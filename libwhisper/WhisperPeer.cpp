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
/** @file WhisperPeer.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "WhisperPeer.h"

#include <libdevcore/Log.h>
#include <libp2p/All.h>
#include "WhisperHost.h"
using namespace std;
using namespace dev;
using namespace dev::p2p;
using namespace dev::shh;
#define clogS(X) dev::LogOutputStream<X, true>(false) << "| " << std::setw(2) << session()->socketId() << "] "

WhisperPeer::WhisperPeer(Session* _s, HostCapabilityFace* _h): Capability(_s, _h)
{
	RLPStream s;
	prep(s);
	sealAndSend(s.appendList(2) << StatusPacket << host()->protocolVersion());
}

WhisperPeer::~WhisperPeer()
{
}

WhisperHost* WhisperPeer::host() const
{
	return static_cast<WhisperHost*>(Capability::hostCapability());
}

bool WhisperPeer::interpret(RLP const& _r)
{
	switch (_r[0].toInt<unsigned>())
	{
	case StatusPacket:
	{
		auto protocolVersion = _r[1].toInt<unsigned>();

		clogS(NetMessageSummary) << "Status: " << protocolVersion;

		if (protocolVersion != host()->protocolVersion())
			disable("Invalid protocol version.");

		if (session()->id() < host()->host()->id())
			sendMessages();
		break;
	}
	case MessagesPacket:
	{
		unsigned n = 0;
		for (auto i: _r)
			if (n++)
				host()->inject(Message(i), this);
		sendMessages();
		break;
	}
	default:
		return false;
	}
	return true;
}

void WhisperPeer::sendMessages()
{
	RLPStream amalg;
	unsigned n = 0;

	Guard l(x_unseen);
	while (m_unseen.size())
	{
		auto p = *m_unseen.begin();
		m_unseen.erase(m_unseen.begin());
		host()->streamMessage(p.second, amalg);
		n++;
	}

	// pause before sending if no messages to send
	if (!n)
		this_thread::sleep_for(chrono::milliseconds(100));

	RLPStream s;
	prep(s);
	s.appendList(n + 1) << MessagesPacket;
	s.appendRaw(amalg.out(), n);
	sealAndSend(s);
}

void WhisperPeer::noteNewMessage(h256 _h, Message const& _m)
{
	Guard l(x_unseen);
	m_unseen[rating(_m)] = _h;
}
