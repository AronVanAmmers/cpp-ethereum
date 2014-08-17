/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file PeerSession.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "PeerSession.h"

#include <chrono>
#include <libethential/Common.h>
#include <libethcore/Exceptions.h>
#include "BlockChain.h"
#include "PeerServer.h"
using namespace std;
using namespace eth;

#define clogS(X) eth::LogOutputStream<X, true>(false) << "| " << std::setw(2) << m_socket.native_handle() << "] "

PeerSession::PeerSession(PeerServer* _s, bi::tcp::socket _socket, u256 _rNId, bi::address _peerAddress, unsigned short _peerPort):
	m_server(_s),
	m_socket(std::move(_socket)),
	m_reqNetworkId(_rNId),
	m_listenPort(_peerPort),
	m_rating(0)
{
	m_disconnect = std::chrono::steady_clock::time_point::max();
	m_connect = std::chrono::steady_clock::now();
	m_info = PeerInfo({"?", _peerAddress.to_string(), m_listenPort, std::chrono::steady_clock::duration(0)});
}

PeerSession::~PeerSession()
{
	giveUpOnFetch();
}

void PeerSession::giveUpOnFetch()
{
	if (m_askedBlocks.size())
	{
		Guard l (m_server->x_blocksNeeded);
		m_server->m_blocksNeeded.reserve(m_server->m_blocksNeeded.size() + m_askedBlocks.size());
		for (auto i: m_askedBlocks)
		{
			m_server->m_blocksOnWay.erase(i);
			m_server->m_blocksNeeded.push_back(i);
		}
		m_askedBlocks.clear();
	}
}

bi::tcp::endpoint PeerSession::endpoint() const
{
	if (m_socket.is_open())
		try
		{
			return bi::tcp::endpoint(m_socket.remote_endpoint().address(), m_listenPort);
		}
		catch (...){}

	return bi::tcp::endpoint();
}

bool PeerSession::interpret(RLP const& _r)
{
	clogS(NetRight) << _r;
	switch (_r[0].toInt<unsigned>())
	{
	case HelloPacket:
	{
		m_protocolVersion = _r[1].toInt<uint>();
		m_networkId = _r[2].toInt<u256>();
		auto clientVersion = _r[3].toString();
		m_caps = _r[4].toInt<uint>();
		m_listenPort = _r[5].toInt<unsigned short>();
		m_id = _r[6].toHash<h512>();
		m_totalDifficulty = _r[7].toInt<u256>();
		m_latestHash = _r[8].toHash<h256>();

		clogS(NetMessageSummary) << "Hello: " << clientVersion << "V[" << m_protocolVersion << "/" << m_networkId << "]" << m_id.abridged() << showbase << hex << m_caps << dec << m_listenPort;

		if (m_server->havePeer(m_id))
		{
			// Already connected.
			cwarn << "Already have peer id" << m_id.abridged(); // << "at" << p->endpoint() << "rather than" << endpoint();
			sendDisconnect(DuplicatePeer);
			return false;
		}

		if (m_protocolVersion != PeerServer::protocolVersion() || m_networkId != m_server->networkId() || !m_id)
		{
			sendDisconnect(IncompatibleProtocol);
			return false;
		}
		try
			{ m_info = PeerInfo({clientVersion, m_socket.remote_endpoint().address().to_string(), m_listenPort, std::chrono::steady_clock::duration()}); }
		catch (...)
		{
			sendDisconnect(BadProtocol);
			return false;
		}

		m_server->registerPeer(shared_from_this());
		startInitialSync();

		// Grab trsansactions off them.
		{
			RLPStream s;
			prep(s).appendList(1);
			s << GetTransactionsPacket;
			sealAndSend(s);
		}
		break;
	}
	case DisconnectPacket:
	{
		string reason = "Unspecified";
		if (_r[1].isInt())
			reason = reasonOf((DisconnectReason)_r[1].toInt<int>());
		
		// TODO: move into sendDisconnect
		clogS(NetMessageSummary) << "Disconnect (reason: " << reason << ")";
		if (m_socket.is_open())
			clogS(NetConnect) << "Closing " << m_socket.remote_endpoint();
		else
			clogS(NetConnect) << "Remote closed.";
		m_writeQueue.clear();
		m_socket.shutdown(ba::ip::tcp::socket::shutdown_both);
		m_socket.close();
		return false;
	}
	case PingPacket:
	{
        clogS(NetTriviaSummary) << "Ping";
		RLPStream s;
		sealAndSend(prep(s).appendList(1) << PongPacket);
		break;
	}
	case PongPacket:
		m_info.lastPing = std::chrono::steady_clock::now() - m_ping;
        clogS(NetTriviaSummary) << "Latency: " << chrono::duration_cast<chrono::milliseconds>(m_info.lastPing).count() << " ms";
		break;
	case GetPeersPacket:
	{
        clogS(NetTriviaSummary) << "GetPeers";
		auto peers = m_server->potentialPeers();
		RLPStream s;
		prep(s).appendList(peers.size() + 1);
		s << PeersPacket;
		for (auto i: peers)
		{
            clogS(NetTriviaDetail) << "Sending peer " << toHex(i.first.ref().cropped(0, 4)) << i.second;
			s.appendList(3) << bytesConstRef(i.second.address().to_v4().to_bytes().data(), 4) << i.second.port() << i.first;
		}
		sealAndSend(s);
		break;
	}
	case PeersPacket:
        clogS(NetTriviaSummary) << "Peers (" << dec << (_r.itemCount() - 1) << " entries)";
		for (unsigned i = 1; i < _r.itemCount(); ++i)
		{
			bi::address_v4 peerAddress(_r[i][0].toHash<FixedHash<4>>().asArray());
			auto ep = bi::tcp::endpoint(peerAddress, _r[i][1].toInt<short>());
			Public id = _r[i][2].toHash<Public>();
			if (isPrivateAddress(peerAddress))
				goto CONTINUE;

			clogS(NetAllDetail) << "Checking: " << ep << "(" << toHex(id.ref().cropped(0, 4)) << ")";

			// check that it's not us or one we already know:
			if (id && (m_server->m_key.pub() == id || m_server->havePeer(id) || m_server->m_incomingPeers.count(id)))
				goto CONTINUE;

			// check that we're not already connected to addr:
			if (!ep.port())
				goto CONTINUE;
			for (auto i: m_server->m_addresses)
				if (ep.address() == i && ep.port() == m_server->listenPort())
					goto CONTINUE;
			for (auto i: m_server->m_incomingPeers)
				if (i.second.first == ep)
					goto CONTINUE;
			m_server->m_incomingPeers[id] = make_pair(ep, 0);
			m_server->m_freePeers.push_back(id);
			m_server->noteNewPeers();
            clogS(NetTriviaDetail) << "New peer: " << ep << "(" << id << ")";
			CONTINUE:;
		}
		break;
	case TransactionsPacket:
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		clogS(NetMessageSummary) << "Transactions (" << dec << (_r.itemCount() - 1) << " entries)";
		m_rating += _r.itemCount() - 1;
		for (unsigned i = 1; i < _r.itemCount(); ++i)
		{
			m_server->m_incomingTransactions.push_back(_r[i].data().toBytes());
			m_knownTransactions.insert(sha3(_r[i].data()));
		}
		break;
	case GetBlockHashesPacket:
	{
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		unsigned limit = _r[1].toInt<unsigned>();
		h256 later = _r[1].toHash<h256>();
		clogS(NetMessageSummary) << "GetBlockHashes (" << limit << "entries, " << later << ")";

		unsigned c = min<unsigned>(m_server->m_chain->number(later), limit);

		RLPStream s;
		prep(s).appendList(1 + c).append(BlockHashesPacket);
		h256 p = m_server->m_chain->details(later).parent;
		for (unsigned i = 0; i < c; ++i, p = m_server->m_chain->details(p).parent)
			s << p;
		sealAndSend(s);
	}
	case BlockHashesPacket:
	{
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		clogS(NetMessageSummary) << "BlockHashes (" << dec << (_r.itemCount() - 1) << " entries)";
		for (unsigned i = 1; i < _r.itemCount() && !m_server->m_chain->details(_r[i].toHash<h256>()); ++i)
		{
			auto h = _r[i].toHash<h256>();
			if (m_server->m_chain->details(h))
			{
				m_server->noteHaveChain(shared_from_this());
				return true;
			}
			else
				m_neededBlocks.push_back(h);
		}
		// run through - ask for more.
		RLPStream s;
		prep(s).appendList(3);
		s << GetBlockHashesPacket << c_maxHashesAsk << m_neededBlocks.back();
		sealAndSend(s);
	}
	case GetBlocksPacket:
	{
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		clogS(NetMessageSummary) << "GetBlocks (" << dec << (_r.itemCount() - 1) << " entries)";
		// TODO: return the requested blocks.
		bytes rlp;
		unsigned n = 0;
		for (unsigned i = 1; i < _r.itemCount() && i <= c_maxBlocks; ++i)
		{
			auto b = m_server->m_chain->block(_r[i].toHash<h256>());
			if (b.size())
			{
				rlp += b;
				++n;
			}
		}
		RLPStream s;
		sealAndSend(prep(s).appendList(n + 1).append(BlocksPacket).appendRaw(rlp));
	}
	case BlocksPacket:
	{
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		clogS(NetMessageSummary) << "Blocks (" << dec << (_r.itemCount() - 1) << " entries)";

		if (_r.itemCount() == 1)
		{
			// Couldn't get any from last batch - probably got to this peer's latest block - just give up.
			giveUpOnFetch();
			break;
		}

		unsigned used = 0;
		for (unsigned i = 1; i < _r.itemCount(); ++i)
		{
			auto h = sha3(_r[i].data());
			if (m_server->noteBlock(h, _r[i].data()))
				used++;
			
			m_askedBlocks.erase(h);
			m_knownBlocks.insert(h);
		}
		m_rating += used;

		if (g_logVerbosity >= NetMessageSummary::verbosity)
		{
			unsigned knownParents = 0;
			unsigned unknownParents = 0;
			
			for (unsigned i = 1; i < _r.itemCount(); ++i)
			{
				auto h = sha3(_r[i].data());
				BlockInfo bi(_r[i].data());
				if (!m_server->m_chain->details(bi.parentHash) && !m_knownBlocks.count(bi.parentHash))
				{
					unknownParents++;
					clogS(NetAllDetail) << "Unknown parent " << bi.parentHash << " of block " << h;
				}
				else
				{
					knownParents++;
					clogS(NetAllDetail) << "Known parent " << bi.parentHash << " of block " << h;
				}
			}
			
			clogS(NetMessageSummary) << dec << knownParents << " known parents, " << unknownParents << "unknown, " << used << "used.";
		}

		ensureGettingChain();
		break;
	}
	case GetTransactionsPacket:
	{
		if (m_server->m_mode == NodeMode::PeerServer)
			break;
		m_requireTransactions = true;
		break;
	}
	default:
		break;
	}
	return true;
}

void PeerSession::ensureGettingChain()
{
	if (!m_askedBlocks.size())
		m_askedBlocks = m_server->neededBlocks();

	if (m_askedBlocks.size())
	{
		RLPStream s;
		prep(s);
		s.appendList(m_askedBlocks.size() + 1) << GetBlocksPacket << m_askedBlocks;
		sealAndSend(s);
	}
	else
		clogS(NetMessageSummary) << "No blocks left to get.";
}

void PeerSession::ping()
{
	// TODO: ping timestamp to be sent when packet is sent rather than when dispatched
	RLPStream s;
	sealAndSend(prep(s).appendList(1) << PingPacket);
	m_ping = std::chrono::steady_clock::now();
}

void PeerSession::getPeers()
{
	RLPStream s;
	sealAndSend(prep(s).appendList(1) << GetPeersPacket);
}

RLPStream& PeerSession::prep(RLPStream& _s)
{
	return _s.appendRaw(bytes(8, 0));
}

void PeerSession::sealAndSend(RLPStream& _s)
{
	bytes b;
	_s.swapOut(b);
	m_server->seal(b);
	sendDestroy(b);
}

bool PeerSession::checkPacket(bytesConstRef _msg)
{
	if (_msg.size() < 8)
		return false;
	if (!(_msg[0] == 0x22 && _msg[1] == 0x40 && _msg[2] == 0x08 && _msg[3] == 0x91))
		return false;
	uint32_t len = ((_msg[4] * 256 + _msg[5]) * 256 + _msg[6]) * 256 + _msg[7];
	if (_msg.size() != len + 8)
		return false;
	RLP r(_msg.cropped(8));
	if (r.actualSize() != len)
		return false;
	return true;
}

void PeerSession::sendDestroy(bytes& _msg)
{
	clogS(NetLeft) << RLP(bytesConstRef(&_msg).cropped(8));

	if (!checkPacket(bytesConstRef(&_msg)))
	{
		cwarn << "INVALID PACKET CONSTRUCTED!";
	}
	
	bytes *buffer = new bytes(std::move(_msg));
	auto self(shared_from_this());
	m_socket.get_io_service().post([this, buffer, self]{
		doWrite(*buffer);
		delete buffer;
	});
}

void PeerSession::send(bytesConstRef _msg)
{
	clogS(NetLeft) << RLP(_msg.cropped(8));
	
	if (!checkPacket(_msg))
	{
		cwarn << "INVALID PACKET CONSTRUCTED!";
	}
	
	bytes *buffer = new bytes(_msg.toBytes());
	auto self(shared_from_this());
	m_socket.get_io_service().post([this, buffer, self]{
		doWrite(*buffer);
		delete buffer;
	});
}

void PeerSession::doWrite(bytes& _buffer)
{
	// boost docs recommend bool here
	bool write_in_progress = !m_writeQueue.empty();
	m_writeQueue.push_back(_buffer);
	if(!write_in_progress)
	{
		auto self(shared_from_this());
		ba::async_write(m_socket, ba::buffer(m_writeQueue.front()), [this, self](const boost::system::error_code& _ec, std::size_t /*len*/){
			handleWrite(_ec);
		});
	}
}

void PeerSession::handleWrite(const boost::system::error_code& _ec) {
	if (_ec)
	{
		// TODO: pass error to sendisconnect
		cwarn << "Error sending: " << _ec.message();
		sendDisconnect(TCPError);
	}
	else
		// handleWrite is recursively called via callback and is meant to remove the
		// previously-interpreted buffer. however it's possible that the queue gets
		// cleared by a disconnect. thus it's necessary to check twice if the buffer is empty.
		if (!m_writeQueue.empty())
			m_writeQueue.pop_front(); // pop previous buffer
		if (!m_writeQueue.empty())
		{
			auto self(shared_from_this());
			boost::asio::async_write(m_socket, ba::buffer(m_writeQueue.front()), [this, self](const boost::system::error_code& _ec, std::size_t /*len*/){
				handleWrite(_ec);
			});
		}
}

//- Ensure socket is available and peer is not scheduled for disconnect. If false is returned, peer will ensure it is safe to delete.
bool PeerSession::ensureOpen()
{
	// return socket status if connection hasn't timed out
	if (m_disconnect == chrono::steady_clock::time_point::max() || chrono::steady_clock::now() - m_disconnect < chrono::seconds(2))
		return m_socket.is_open();

	// otherwise kill timed-out connection
	if (m_socket.is_open())
		try
		{
			clogS(NetConnect) << "Closing " << m_socket.remote_endpoint();
			m_writeQueue.clear();
			m_socket.shutdown(ba::ip::tcp::socket::shutdown_both);
			m_socket.close();
		}
		catch (...) {}
	return false;
}

void PeerSession::sendDisconnect(DisconnectReason _reason)
{
	m_writeQueue.clear();
	if (!ensureOpen())
		return;
	
	m_disconnect = chrono::steady_clock::now();
	
	clogS(NetConnect) << "Disconnecting (reason:" << reasonOf((DisconnectReason)_reason) << ")";
	if (_reason == ClientQuit || _reason == TCPError || !m_socket.is_open())
	{
		// Disconnect immediately if closed socket, TPCError, or ClientQuit
		if (m_socket.is_open())
			try
			{
				clogS(NetConnect) << "Closing " << m_socket.remote_endpoint();
				m_socket.shutdown(ba::ip::tcp::socket::shutdown_both);
				m_socket.close();
			}
			catch (...) {}
		return;
	}

	// Otherwise send disconnect packet
	RLPStream s;
	prep(s);
	s.appendList(2) << DisconnectPacket << _reason;
	sealAndSend(s);
}

void PeerSession::start()
{
	RLPStream s;
	prep(s);
	s.appendList(7) << HelloPacket
					<< (uint)PeerServer::protocolVersion()
					<< m_server->networkId()
					<< m_server->m_clientVersion
					<< (m_server->m_mode == NodeMode::Full ? 0x07 : m_server->m_mode == NodeMode::PeerServer ? 0x01 : 0)
					<< m_server->m_public.port()
					<< m_server->m_key.pub()
					<< m_server->m_chain->currentHash()
					<< m_server->m_chain->details().totalDifficulty;
	sealAndSend(s);
	ping();
	getPeers();

	doRead();
}

void PeerSession::startInitialSync()
{
	h256 c = m_server->m_chain->currentHash();
	uint n = m_server->m_chain->number();
	u256 td = max(m_server->m_chain->details().totalDifficulty, m_server->m_totalDifficultyOfNeeded);

	clogS(NetAllDetail) << "Initial sync. Latest:" << c.abridged() << ", number:" << n << ", TD: max(" << m_server->m_chain->details().totalDifficulty << "," << m_server->m_totalDifficultyOfNeeded << ") versus " << m_totalDifficulty;
	if (td > m_totalDifficulty)
		return;	// All good - we have the better chain.

	// Our chain isn't better - grab theirs.
	RLPStream s;
	prep(s).appendList(3);
	s << GetBlockHashesPacket << c_maxHashesAsk << m_latestHash;
	m_neededBlocks = h256s(1, m_latestHash);
	sealAndSend(s);
}

void PeerSession::doRead()
{
	auto self(shared_from_this());
	m_socket.async_read_some(boost::asio::buffer(m_data), [this, self](boost::system::error_code _ec, std::size_t _length)
	{
		handleRead(_ec, _length);
	});
}

void PeerSession::handleRead(const boost::system::error_code& _ec, size_t _length)
{
	if (!ensureOpen())
		return;
	
	// If error is end of file, ignore
	if (_ec && _ec.category() != boost::asio::error::get_misc_category() && _ec.value() != boost::asio::error::eof)
	{
		cwarn << "Error reading: " << _ec.message();
		sendDisconnect(BadProtocol);
	}
	else if (_ec && _length == 0)
		return;
	else
		try
		{
			m_incoming.resize(m_incoming.size() + _length);
			memcpy(m_incoming.data() + m_incoming.size() - _length, m_data.data(), _length);
			while (m_incoming.size() > 8)
			{
				if (m_incoming[0] != 0x22 || m_incoming[1] != 0x40 || m_incoming[2] != 0x08 || m_incoming[3] != 0x91)
				{
					cwarn << "INVALID MESSAGE RECEIVED";
					sendDisconnect(BadProtocol);
					return;
				}
				else
				{
					uint32_t len = fromBigEndian<uint32_t>(bytesConstRef(m_incoming.data() + 4, 4));
					uint32_t tlen = len + 8;
					if (m_incoming.size() < tlen)
						break;
					
					// enough has come in.
					auto data = bytesConstRef(m_incoming.data(), tlen);
					if (!checkPacket(data))
					{
						cerr << "Received " << len << ": " << toHex(bytesConstRef(m_incoming.data() + 8, len)) << endl;
						cwarn << "INVALID MESSAGE RECEIVED";
						sendDisconnect(BadProtocol);
						return;
					}
					else
					{
						RLP r(data.cropped(8));
						if (!interpret(r))
						{
							// error
							sendDisconnect(BadProtocol);
							return;
						}
					}
					memmove(m_incoming.data(), m_incoming.data() + tlen, m_incoming.size() - tlen);
					m_incoming.resize(m_incoming.size() - tlen);
				}
			}
			
			auto self(shared_from_this());
			m_socket.async_read_some(boost::asio::buffer(m_data), [this, self](boost::system::error_code _ec, std::size_t _length)
			{
				handleRead(_ec, _length);
			});
		}
		catch (Exception const& _e)
		{
			clogS(NetWarn) << "ERROR: " << _e.description();
			sendDisconnect(BadProtocol);
		}
		catch (std::exception const& _e)
		{
			clogS(NetWarn) << "ERROR: " << _e.what();
			sendDisconnect(BadProtocol);
		}
}
