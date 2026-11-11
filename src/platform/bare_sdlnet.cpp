//
// bare_sdlnet.cpp
//
// BM-DOS - SDL_net for DOSBox's modem and null-modem (and IPX next), on
// Circle's TCP/IP stack: CSocket, CDNSClient.  The interface is
// src/dosbox-stub/SDL_net.h; only what DOSBox uses is here.
//
// Three things SDL_net does that Circle's sockets do not, and how they are
// done here:
//
//   * "Is there data?" without reading it (SDLNet_CheckSockets).  Each socket
//     keeps a small receive buffer; checking tries a non-blocking Receive
//     into it.  Data waiting, or the peer having closed, is "ready" - DOSBox
//     then reads, and a read of <= 0 is how it learns the line dropped.
//   * A non-blocking accept (SDLNet_TCP_Accept, polled by the modem for an
//     incoming call).  Circle's Accept blocks, so a listening socket gets a
//     scheduler task that waits in Accept and parks the connection it gets;
//     SDLNet_TCP_Accept hands it over.  A listening socket is never deleted:
//     a task blocked in its Accept cannot be told to stop, and deleting the
//     socket under it would crash.  It is marked closed instead, and what it
//     accepts afterwards is dropped.  There is one per configured listen
//     port, for the whole session.
//   * Connecting and resolving a name block the DOSBox thread, as a dial
//     does on a real modem: Circle's Connect and Resolve wait in the
//     scheduler, which keeps the network's own tasks and the frontend
//     running meanwhile.
//
// Without a network (boot.conf network = off, or not up yet) every open fails
// with a reason in SDLNet_GetError, and DOSBox's modem answers NO CARRIER.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"
#include "SDL_net.h"

#include <circle/logger.h>
#include <circle/net/dnsclient.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>

#include <cstdio>
#include <cstring>

static const char FromNet[] = "sdlnet";

struct _TCPsocket
{
	int ready;			// first, as every SDL_net socket (SDLNet_SocketReady)
	CSocket *pSocket;
	bool bServer;
	bool bClosed;			// the peer closed, or an error: reads return 0
	IPaddress Peer;

	// Received but not yet read (see CheckSockets).
	u8 Buffer[2048];
	unsigned nHead;
	unsigned nTail;

	// Server: the connection the accept task parked, and whether the
	// socket was closed by DOSBox (it stays alive underneath, see above).
	_TCPsocket *pPending;
	bool bAbandoned;
};

struct _SDLNet_SocketSet
{
	int nMax;
	int nCount;
	SDLNet_GenericSocket *pSockets;
};

namespace {

char s_Error[96] = "";

void SetError (const char *pText)
{
	snprintf (s_Error, sizeof s_Error, "%s", pText);
	CLogger::Get ()->Write (FromNet, LogNotice, "%s", pText);
}

CNetSubSystem *Net (void)
{
	if (!BareNetSocketsReady ())
	{
		SetError ("la red de la placa no está conectada (menú Win+F12, Red)");
		return nullptr;
	}
	return CNetSubSystem::Get ();
}

_TCPsocket *NewSocket (CSocket *pSocket, bool bServer)
{
	_TCPsocket *p = new _TCPsocket;
	memset (p, 0, sizeof *p);
	p->pSocket = pSocket;
	p->bServer = bServer;
	return p;
}

IPaddress ToIPaddress (const u8 *pIP, u16 nPort)
{
	// Network byte order in memory, as SDL_net: the first octet first.
	IPaddress Address;
	memcpy (&Address.host, pIP, 4);
	Address.port = static_cast<Uint16> ((nPort >> 8) | (nPort << 8));
	return Address;
}

u16 HostPort (const IPaddress *pAddress)
{
	return static_cast<u16> ((pAddress->port >> 8) | (pAddress->port << 8));
}

// Fill the receive buffer without waiting.  Returns true if there is
// something to read (data, or the news that the connection is gone).
bool Poll (_TCPsocket *p)
{
	if (p->nHead != p->nTail || p->bClosed)
	{
		return true;
	}
	p->nHead = p->nTail = 0;
	const int n = p->pSocket->Receive (p->Buffer, sizeof p->Buffer, MSG_DONTWAIT);
	if (n > 0)
	{
		p->nTail = static_cast<unsigned> (n);
		return true;
	}
	if (n < 0)
	{
		p->bClosed = true;
		return true;
	}
	return false;
}

// Waits in Accept for a listening socket and parks what it gets.
class CAcceptTask : public CTask
{
public:
	explicit CAcceptTask (_TCPsocket *pServer) : m_pServer (pServer) {}

	void Run (void) override
	{
		for (;;)
		{
			CIPAddress ForeignIP;
			u16 nForeignPort = 0;
			CSocket *pConnection = m_pServer->pSocket->Accept (&ForeignIP, &nForeignPort);
			if (pConnection == nullptr)
			{
				CScheduler::Get ()->MsSleep (100);
				continue;
			}

			// One call at a time, as a modem line; and nothing for a
			// server DOSBox has closed.
			if (m_pServer->bAbandoned || m_pServer->pPending != nullptr)
			{
				delete pConnection;
				continue;
			}

			_TCPsocket *p = NewSocket (pConnection, false);
			u8 IP[4];
			ForeignIP.CopyTo (IP);
			p->Peer = ToIPaddress (IP, nForeignPort);
			m_pServer->pPending = p;
			m_pServer->ready = 1;
		}
	}

private:
	_TCPsocket *m_pServer;
};

} // namespace

// ---------------------------------------------------------------------------
// The interface (C linkage not needed: DOSBox includes SDL_net.h from C++)
// ---------------------------------------------------------------------------

int SDLNet_Init (void)
{
	return 0;	// nothing to set up; opening a socket checks the network
}

void SDLNet_Quit (void)
{
}

const char *SDLNet_GetError (void)
{
	return s_Error;
}

int SDLNet_ResolveHost (IPaddress *pAddress, const char *pHost, Uint16 nPort)
{
	if (pAddress == nullptr)
	{
		return -1;
	}

	const Uint16 nNetPort = static_cast<Uint16> ((nPort >> 8) | (nPort << 8));
	if (pHost == nullptr)
	{
		pAddress->host = INADDR_ANY;
		pAddress->port = nNetPort;
		return 0;
	}

	// A dotted address needs no DNS.
	unsigned a, b, c, d;
	char cExtra;
	if (sscanf (pHost, "%u.%u.%u.%u%c", &a, &b, &c, &d, &cExtra) == 4
	    && a < 256 && b < 256 && c < 256 && d < 256)
	{
		const u8 IP[4] = { static_cast<u8> (a), static_cast<u8> (b), static_cast<u8> (c), static_cast<u8> (d) };
		*pAddress = ToIPaddress (IP, nPort);
		return 0;
	}

	CNetSubSystem *pNet = Net ();
	if (pNet == nullptr)
	{
		return -1;
	}

	CDNSClient DNS (pNet);
	CIPAddress IP;
	if (!DNS.Resolve (pHost, &IP))
	{
		SetError ("no se encontró ese nombre (DNS)");
		return -1;
	}
	u8 Bytes[4];
	IP.CopyTo (Bytes);
	*pAddress = ToIPaddress (Bytes, nPort);
	return 0;
}

TCPsocket SDLNet_TCP_Open (IPaddress *pAddress)
{
	CNetSubSystem *pNet = Net ();
	if (pNet == nullptr || pAddress == nullptr)
	{
		return nullptr;
	}

	CSocket *pSocket = new CSocket (pNet, IPPROTO_TCP);
	if (pSocket == nullptr)
	{
		return nullptr;
	}

	if (pAddress->host == INADDR_ANY)
	{
		// A server: listen, and let a task wait in Accept.
		if (pSocket->Bind (HostPort (pAddress)) < 0 || pSocket->Listen () < 0)
		{
			delete pSocket;
			SetError ("no se pudo escuchar en ese puerto");
			return nullptr;
		}
		_TCPsocket *p = NewSocket (pSocket, true);
		new CAcceptTask (p);
		CLogger::Get ()->Write (FromNet, LogNotice, "listening on port %u", HostPort (pAddress));
		return p;
	}

	u8 Bytes[4];
	memcpy (Bytes, &pAddress->host, 4);
	CIPAddress IP (Bytes);
	if (pSocket->Connect (IP, HostPort (pAddress)) < 0)
	{
		delete pSocket;
		SetError ("no contesta (conexión rechazada o sin ruta)");
		return nullptr;
	}

	_TCPsocket *p = NewSocket (pSocket, false);
	p->Peer = *pAddress;
	CLogger::Get ()->Write (FromNet, LogNotice, "connected to %u.%u.%u.%u:%u",
				Bytes[0], Bytes[1], Bytes[2], Bytes[3], HostPort (pAddress));
	return p;
}

TCPsocket SDLNet_TCP_Accept (TCPsocket pServer)
{
	if (pServer == nullptr || !pServer->bServer)
	{
		return nullptr;
	}
	_TCPsocket *p = pServer->pPending;
	pServer->pPending = nullptr;
	pServer->ready = 0;
	return p;
}

IPaddress *SDLNet_TCP_GetPeerAddress (TCPsocket p)
{
	return p != nullptr && !p->bServer ? &p->Peer : nullptr;
}

int SDLNet_TCP_Send (TCPsocket p, const void *pData, int nLength)
{
	if (p == nullptr || p->bServer || p->bClosed || nLength <= 0)
	{
		return nLength <= 0 ? 0 : -1;
	}
	const int n = p->pSocket->Send (pData, static_cast<unsigned> (nLength), 0);
	if (n < 0)
	{
		p->bClosed = true;
	}
	return n;
}

int SDLNet_TCP_Recv (TCPsocket p, void *pData, int nMax)
{
	if (p == nullptr || p->bServer || nMax <= 0)
	{
		return -1;
	}

	if (p->nHead == p->nTail && !p->bClosed)
	{
		// Nothing buffered: SDL_net's Recv blocks, so does this.
		const int n = p->pSocket->Receive (p->Buffer, sizeof p->Buffer, 0);
		if (n <= 0)
		{
			p->bClosed = true;
			return 0;
		}
		p->nHead = 0;
		p->nTail = static_cast<unsigned> (n);
	}

	if (p->nHead == p->nTail)
	{
		return 0;		// closed
	}

	unsigned nCopy = p->nTail - p->nHead;
	if (nCopy > static_cast<unsigned> (nMax))
	{
		nCopy = static_cast<unsigned> (nMax);
	}
	memcpy (pData, p->Buffer + p->nHead, nCopy);
	p->nHead += nCopy;
	p->ready = p->nHead != p->nTail;
	return static_cast<int> (nCopy);
}

void SDLNet_TCP_Close (TCPsocket p)
{
	if (p == nullptr)
	{
		return;
	}
	if (p->bServer)
	{
		// See the top of the file: abandoned, not deleted.
		p->bAbandoned = true;
		if (p->pPending != nullptr)
		{
			SDLNet_TCP_Close (p->pPending);
			p->pPending = nullptr;
		}
		return;
	}
	delete p->pSocket;
	delete p;
}

SDLNet_SocketSet SDLNet_AllocSocketSet (int nMax)
{
	if (nMax <= 0)
	{
		return nullptr;
	}
	_SDLNet_SocketSet *pSet = new _SDLNet_SocketSet;
	pSet->nMax = nMax;
	pSet->nCount = 0;
	pSet->pSockets = new SDLNet_GenericSocket[nMax];
	return pSet;
}

int SDLNet_AddSocket (SDLNet_SocketSet pSet, SDLNet_GenericSocket pSocket)
{
	if (pSet == nullptr || pSocket == nullptr || pSet->nCount >= pSet->nMax)
	{
		return -1;
	}
	pSet->pSockets[pSet->nCount++] = pSocket;
	return pSet->nCount;
}

int SDLNet_DelSocket (SDLNet_SocketSet pSet, SDLNet_GenericSocket pSocket)
{
	if (pSet == nullptr)
	{
		return -1;
	}
	for (int i = 0; i < pSet->nCount; i++)
	{
		if (pSet->pSockets[i] == pSocket)
		{
			pSet->pSockets[i] = pSet->pSockets[--pSet->nCount];
			return pSet->nCount;
		}
	}
	return -1;
}

int SDLNet_CheckSockets (SDLNet_SocketSet pSet, Uint32 nTimeoutMs)
{
	if (pSet == nullptr)
	{
		return -1;
	}

	unsigned nWaited = 0;
	for (;;)
	{
		int nReady = 0;
		for (int i = 0; i < pSet->nCount; i++)
		{
			// Only TCP sockets are put in sets by DOSBox's serial code.
			_TCPsocket *p = reinterpret_cast<_TCPsocket *> (pSet->pSockets[i]);
			p->ready = p->bServer ? p->pPending != nullptr : Poll (p);
			nReady += p->ready ? 1 : 0;
		}
		if (nReady != 0 || nWaited >= nTimeoutMs)
		{
			return nReady;
		}
		CScheduler::Get ()->MsSleep (1);
		nWaited++;
	}
}

void SDLNet_FreeSocketSet (SDLNet_SocketSet pSet)
{
	if (pSet == nullptr)
	{
		return;
	}
	delete[] pSet->pSockets;
	delete pSet;
}
