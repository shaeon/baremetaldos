//
// SDL_net.h - the part of SDL_net that DOSBox's modem, null-modem and IPX use,
// for BM-DOS: implemented in src/platform/bare_sdlnet.cpp on Circle's TCP/IP
// stack (bare_net.cpp brings the board on the network).  Same names and
// semantics as SDL_net 1.2 for what is here; nothing else is.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_SDL_NET_H
#define BARE_SDL_NET_H

#include <stdint.h>

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;

/// host and port in network byte order, as SDL_net keeps them.
typedef struct
{
	Uint32 host;
	Uint16 port;
} IPaddress;

#ifndef INADDR_ANY
#define INADDR_ANY		0x00000000
#endif
#ifndef INADDR_NONE
#define INADDR_NONE		0xFFFFFFFF
#endif
#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST	0xFFFFFFFF
#endif

typedef struct _TCPsocket *TCPsocket;
typedef struct _UDPsocket *UDPsocket;
typedef struct _SDLNet_SocketSet *SDLNet_SocketSet;

/// Every socket starts with this, as in SDL_net.
typedef struct _SDLNet_GenericSocket
{
	int ready;
} *SDLNet_GenericSocket;

#define SDLNet_SocketReady(sock) \
	((sock) != NULL && ((SDLNet_GenericSocket) (sock))->ready)

int  SDLNet_Init (void);
void SDLNet_Quit (void);
const char *SDLNet_GetError (void);

/// host NULL: a server address (INADDR_ANY).  0 on success, -1 on failure.
int SDLNet_ResolveHost (IPaddress *address, const char *host, Uint16 port);

/// A client connection to ip, or (ip->host INADDR_ANY) a listening server.
TCPsocket SDLNet_TCP_Open (IPaddress *ip);
/// A pending connection on a server socket, or NULL; never blocks.
TCPsocket SDLNet_TCP_Accept (TCPsocket server);
IPaddress *SDLNet_TCP_GetPeerAddress (TCPsocket sock);
int  SDLNet_TCP_Send (TCPsocket sock, const void *data, int len);
/// Blocks until something arrives, unless SDLNet_CheckSockets said it was
/// ready; <= 0 when the connection is closed.
int  SDLNet_TCP_Recv (TCPsocket sock, void *data, int maxlen);
void SDLNet_TCP_Close (TCPsocket sock);

SDLNet_SocketSet SDLNet_AllocSocketSet (int maxsockets);
int  SDLNet_AddSocket (SDLNet_SocketSet set, SDLNet_GenericSocket sock);
int  SDLNet_DelSocket (SDLNet_SocketSet set, SDLNet_GenericSocket sock);
#define SDLNet_TCP_AddSocket(set, sock)	SDLNet_AddSocket (set, (SDLNet_GenericSocket) (sock))
#define SDLNet_TCP_DelSocket(set, sock)	SDLNet_DelSocket (set, (SDLNet_GenericSocket) (sock))
/// Number of sockets with data (or closed); timeout in ms, 0 = do not wait.
int  SDLNet_CheckSockets (SDLNet_SocketSet set, Uint32 timeout);
void SDLNet_FreeSocketSet (SDLNet_SocketSet set);

// --- UDP (DOSBox's IPX over UDP: ipx.cpp, ipxserver.cpp) ----------------------

typedef struct
{
	int channel;		// the channel it came from / goes to; -1: use address
	Uint8 *data;
	int len;
	int maxlen;
	int status;
	IPaddress address;	// sender on receive, destination with channel -1
} UDPpacket;

#define SDLNET_MAX_UDPCHANNELS		32
#define SDLNET_MAX_UDPADDRESSES		4

/// A UDP socket bound to port (0: any free port).
UDPsocket SDLNet_UDP_Open (Uint16 port);
/// Bind an address to a channel (-1: the first free one).  The channel, or -1.
int  SDLNet_UDP_Bind (UDPsocket sock, int channel, const IPaddress *address);
void SDLNet_UDP_Unbind (UDPsocket sock, int channel);
/// Send to the channel's addresses, or (channel -1) to packet->address.
/// The number of destinations it went to (0 on failure).
int  SDLNet_UDP_Send (UDPsocket sock, int channel, UDPpacket *packet);
/// One datagram if there is one: 1, 0 if none (never waits), -1 on error.
int  SDLNet_UDP_Recv (UDPsocket sock, UDPpacket *packet);
void SDLNet_UDP_Close (UDPsocket sock);

// Big-endian 16/32-bit fields, as the IPX code reads and writes them.
#define SDLNet_Write16(value, areap) \
	do { Uint8 *p_ = (Uint8 *) (areap); Uint16 v_ = (Uint16) (value); p_[0] = v_ >> 8; p_[1] = v_ & 0xFF; } while (0)
#define SDLNet_Write32(value, areap) \
	do { Uint8 *p_ = (Uint8 *) (areap); Uint32 v_ = (Uint32) (value); \
	     p_[0] = v_ >> 24; p_[1] = (v_ >> 16) & 0xFF; p_[2] = (v_ >> 8) & 0xFF; p_[3] = v_ & 0xFF; } while (0)
#define SDLNet_Read16(areap) \
	((Uint16) ((((const Uint8 *) (areap))[0] << 8) | ((const Uint8 *) (areap))[1]))
#define SDLNet_Read32(areap) \
	((Uint32) (((Uint32) ((const Uint8 *) (areap))[0] << 24) | ((Uint32) ((const Uint8 *) (areap))[1] << 16) \
		   | ((Uint32) ((const Uint8 *) (areap))[2] << 8) | ((const Uint8 *) (areap))[3]))

#endif
