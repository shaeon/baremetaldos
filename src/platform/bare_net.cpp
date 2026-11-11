//
// bare_net.cpp
//
// BM-DOS - the board's own network: the Ethernet port or WiFi, with Circle's
// TCP/IP stack (lib/net: DHCP, DNS, sockets) and, for WiFi, its WLAN driver
// and wpa_supplicant (addon/wlan).  This is the board on the network; what DOS
// sees of it (a serial modem to telnet, an NE2000, IPX) comes on top of it.
//
//   boot.conf  network = off | ethernet | wifi      (off by default)
//              hostname = bmdos
//              ip = dhcp | 192.168.1.50/24, gateway = ..., dns = ...
//   WiFi       SD:/wpa_supplicant.conf  - the network and its password, in
//                                         the standard format
//              SD:/firmware/            - the chip's firmware (the card built
//                                         on GitHub has it; Circle's
//                                         addon/wlan/firmware fetches it)
//
// The boot does not wait for the network: the stack is started without waiting
// for the link, and associating with the access point and getting an address
// from DHCP happen in Circle's scheduler tasks while DOS already runs - they get
// their time on every Yield() of the frame loop (input polling, DOSBox's thread
// switches).  The setup menu's Red page shows how far it got.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"

#include <circle/logger.h>
#include <circle/macaddress.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netconfig.h>
#include <circle/net/netdevlayer.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ntpdaemon.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle_glue.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static const char FromNet[] = "net";

namespace {

const char *const FirmwarePath = "SD:/firmware/";
const char *const WPAConfigPath = "SD:/wpa_supplicant.conf";

TBareNetMode s_Mode = BareNetOff;
CBcm4343Device *s_pWLAN = nullptr;
CNetSubSystem *s_pNet = nullptr;
CWPASupplicant *s_pWPA = nullptr;
bool s_bDHCP = true;

CNTPDaemon *s_pNTP = nullptr;
bool s_bSockets = false;		// circle-newlib's BSD sockets are wired up

// Any time Circle reports after this is a real one (the board has no clock:
// until NTP answers, time starts at 1970).  2024-01-01.
constexpr unsigned ValidTimeFloor = 1704067200;

// The reason the network is not up, when it failed to start ("" = no failure).
char s_Failure[96] = "";

// Kept for CNetSubSystem, which keeps the pointer.
char s_Hostname[32] = "bmdos";
u8 s_IP[4], s_Mask[4], s_Gateway[4], s_DNS[4];

void Fail (const char *pText)
{
	snprintf (s_Failure, sizeof s_Failure, "%s", pText);
	CLogger::Get ()->Write (FromNet, LogError, "%s", pText);
}

void FormatIP (const u8 *pAddr, char *pOut, size_t nSize)
{
	snprintf (pOut, nSize, "%u.%u.%u.%u", pAddr[0], pAddr[1], pAddr[2], pAddr[3]);
}

// Waits in the scheduler for the stack to come up (link, DHCP), then wires up
// the BSD sockets of circle-newlib (what the modem and IPX use) and starts the
// NTP client.  A task, so that it costs nothing while it waits: it only runs
// when the frame loop yields.
class CNetStarter : public CTask
{
public:
	void Run (void) override
	{
		while (!s_pNet->IsRunning ())
		{
			CScheduler::Get ()->MsSleep (100);
		}

		CGlueNetworkInit (*s_pNet);
		s_bSockets = true;

		// pool.ntp.org: it sets Circle's clock (UTC) now and every 15 min.
		s_pNTP = new CNTPDaemon ("pool.ntp.org", s_pNet);
		CLogger::Get ()->Write (FromNet, LogNotice, "up; sockets ready, NTP started");
	}
};

} // namespace

boolean BareNetInit (const TBareNetConfig *pConfig)
{
	if (pConfig == nullptr || pConfig->Mode == BareNetOff)
	{
		return TRUE;
	}

	s_Mode = pConfig->Mode;
	s_bDHCP = pConfig->bDHCP != FALSE;
	snprintf (s_Hostname, sizeof s_Hostname, "%s", pConfig->pHostname != nullptr ? pConfig->pHostname : "bmdos");
	memcpy (s_IP, pConfig->IP, 4);
	memcpy (s_Mask, pConfig->Mask, 4);
	memcpy (s_Gateway, pConfig->Gateway, 4);
	memcpy (s_DNS, pConfig->DNS, 4);

	if (s_Mode == BareNetWiFi)
	{
		// The chip's driver loads its firmware from the card: a missing
		// /firmware folder is the usual reason this fails.
		s_pWLAN = new CBcm4343Device (FirmwarePath);
		if (s_pWLAN == nullptr || !s_pWLAN->Initialize ())
		{
			Fail ("WiFi: el chip no arranca (falta SD:/firmware/?)");
			return FALSE;
		}
	}

	s_pNet = new CNetSubSystem (s_bDHCP ? nullptr : s_IP,
				    s_bDHCP ? nullptr : s_Mask,
				    s_bDHCP ? nullptr : s_Gateway,
				    s_bDHCP ? nullptr : s_DNS,
				    s_Hostname,
				    s_Mode == BareNetWiFi ? NetDeviceTypeWLAN : NetDeviceTypeEthernet);

	// FALSE: do not wait for the link.  DHCP and the WiFi association finish
	// in scheduler tasks while the boot goes on.
	if (s_pNet == nullptr || !s_pNet->Initialize (FALSE))
	{
		Fail (s_Mode == BareNetWiFi ? "WiFi: la pila de red no arranca"
					    : "Ethernet: no hay puerto de red (¿Zero 2 W?)");
		return FALSE;
	}

	if (s_Mode == BareNetWiFi)
	{
		s_pWPA = new CWPASupplicant (WPAConfigPath);
		if (s_pWPA == nullptr || !s_pWPA->Initialize ())
		{
			Fail ("WiFi: revisa SD:/wpa_supplicant.conf");
			return FALSE;
		}
	}

	new CNetStarter;

	CLogger::Get ()->Write (FromNet, LogNotice, "%s started, %s, hostname %s",
				s_Mode == BareNetWiFi ? "WiFi" : "Ethernet",
				s_bDHCP ? "DHCP" : "fixed address", s_Hostname);
	return TRUE;
}

TBareNetMode BareNetMode (void)
{
	return s_Mode;
}

boolean BareNetReady (void)
{
	return s_pNet != nullptr && s_Failure[0] == '\0' && s_pNet->IsRunning ()
	       && (s_Mode != BareNetWiFi || CWPASupplicant::IsConnected ()) ? TRUE : FALSE;
}

const char *BareNetState (void)
{
	if (s_Mode == BareNetOff)
	{
		return "apagada";
	}
	if (s_Failure[0] != '\0')
	{
		return s_Failure;
	}
	if (s_Mode == BareNetWiFi && !CWPASupplicant::IsConnected ())
	{
		return "WiFi: conectando con la red";
	}
	if (!s_pNet->IsRunning ())
	{
		return s_bDHCP ? "esperando dirección (DHCP)" : "esperando el enlace";
	}
	return "conectada";
}

void BareNetInfo (TBareNetInfo *pInfo)
{
	if (pInfo == nullptr)
	{
		return;
	}
	memset (pInfo, 0, sizeof *pInfo);
	if (s_pNet == nullptr)
	{
		return;
	}

	CNetConfig *pConfig = s_pNet->GetConfig ();
	if (pConfig != nullptr && s_pNet->IsRunning ())
	{
		CString Text;
		pConfig->GetIPAddress ()->Format (&Text);
		snprintf (pInfo->IP, sizeof pInfo->IP, "%s", static_cast<const char *> (Text));
		FormatIP (pConfig->GetNetMask (), pInfo->Mask, sizeof pInfo->Mask);
		pConfig->GetDefaultGateway ()->Format (&Text);
		snprintf (pInfo->Gateway, sizeof pInfo->Gateway, "%s", static_cast<const char *> (Text));
		pConfig->GetDNSServer ()->Format (&Text);
		snprintf (pInfo->DNS, sizeof pInfo->DNS, "%s", static_cast<const char *> (Text));
	}

	CNetDeviceLayer *pLayer = s_pNet->GetNetDeviceLayer ();
	if (pLayer != nullptr && pLayer->GetMACAddress () != nullptr)
	{
		CString Text;
		pLayer->GetMACAddress ()->Format (&Text);
		snprintf (pInfo->MAC, sizeof pInfo->MAC, "%s", static_cast<const char *> (Text));
	}

	snprintf (pInfo->Hostname, sizeof pInfo->Hostname, "%s", s_Hostname);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
//
// DOSBox sets DOS's date and time ONCE, when its BIOS starts
// (BIOS_HostTimeSync: clock_gettime + localtime), so two things must be in
// place by then: the time zone, as newlib's TZ (a POSIX rule, daylight saving
// included), and Circle's clock, from NTP - the board has no clock of its own.

void BareNetSetTimeZone (const char *pPosixTZ)
{
	if (pPosixTZ == nullptr || pPosixTZ[0] == '\0')
	{
		return;
	}
	setenv ("TZ", pPosixTZ, 1);
	tzset ();
}

boolean BareNetTimeValid (void)
{
	return CTimer::Get ()->GetUniversalTime () >= ValidTimeFloor ? TRUE : FALSE;
}

boolean BareNetWaitForTime (unsigned nMaxMillis)
{
	if (s_pNet == nullptr || s_Failure[0] != '\0')
	{
		return BareNetTimeValid ();
	}

	for (unsigned nWaited = 0; nWaited < nMaxMillis && !BareNetTimeValid (); nWaited += 50)
	{
		CScheduler::Get ()->MsSleep (50);
	}
	return BareNetTimeValid ();
}

boolean BareNetSocketsReady (void)
{
	return s_bSockets ? TRUE : FALSE;
}

void BareNetLocalTime (char *pOut, unsigned nSize)
{
	if (!BareNetTimeValid ())
	{
		snprintf (pOut, nSize, "sin hora (falta NTP)");
		return;
	}
	const time_t nNow = static_cast<time_t> (CTimer::Get ()->GetUniversalTime ());
	struct tm Local;
	localtime_r (&nNow, &Local);
	strftime (pOut, nSize, "%d/%m/%Y %H:%M:%S %Z", &Local);
}

// ---------------------------------------------------------------------------
// WiFi networks seen - for the menu's "Elegir red WiFi"
// ---------------------------------------------------------------------------
//
// wpa_supplicant scans on its own (continuously while it has no network to
// join, now and then otherwise), and its driver for Circle
// (addon/wlan/hostap/src/drivers/driver_circle.cpp) collects the results with
// CBcm4343Device::ReceiveScanResult().  A scan of our own would take results
// away from it.  So the Makefile links with --wrap for that one function: every
// call goes through __wrap_ below, which notes the networks in the buffer and
// hands it on untouched.  Nothing in Circle is patched, and with the port's
// code gone the wrap simply calls the real function.
//
// The two structures are copied verbatim from driver_circle.cpp, so that the
// same compiler lays them out the same way.

namespace {

typedef u16 __le16;
typedef u32 __le32;

struct brcmf_bss_info_le
{
	__le32 version;
	__le32 length;
	u8 BSSID[6];
	__le16 beacon_period;
	__le16 capability;
	u8 SSID_len;
	u8 SSID[32];
	struct {
		__le32 count;
		u8 rates[16];
	} rateset;
	__le16 chanspec;
	__le16 atim_window;
	u8 dtim_period;
	__le16 RSSI;
	s8 phy_noise;
	u8 n_cap;
	__le32 nbss_cap;
	u8 ctl_ch;
	__le32 reserved32[1];
	u8 flags;
	u8 reserved[3];
	u8 basic_mcs[16];
	__le16 ie_offset;
	__le32 ie_length;
	__le16 SNR;
};

struct brcmf_escan_result_le
{
	__le32 buflen;
	__le32 version;
	__le16 sync_id;
	__le16 bss_count;
	struct brcmf_bss_info_le bss_info_le;
};

constexpr unsigned MaxSeen = 24;
TBareWiFiNetwork s_Seen[MaxSeen];
unsigned s_nSeen = 0;

void NoteNetwork (const brcmf_bss_info_le *pBSS)
{
	if (pBSS->SSID_len == 0 || pBSS->SSID_len > 32)
	{
		return;				// hidden network: no name to show
	}
	char SSID[33];
	memcpy (SSID, pBSS->SSID, pBSS->SSID_len);
	SSID[pBSS->SSID_len] = '\0';
	const int nRSSI = static_cast<s16> (pBSS->RSSI);
	const bool bSecure = (pBSS->capability & 0x0010) != 0;	// privacy bit

	for (unsigned i = 0; i < s_nSeen; i++)
	{
		if (strcmp (s_Seen[i].SSID, SSID) == 0)
		{
			// Several access points with one name: keep the strongest.
			if (nRSSI > s_Seen[i].nRSSI) s_Seen[i].nRSSI = nRSSI;
			s_Seen[i].bSecure = s_Seen[i].bSecure || bSecure;
			return;
		}
	}
	if (s_nSeen < MaxSeen)
	{
		TBareWiFiNetwork &N = s_Seen[s_nSeen++];
		snprintf (N.SSID, sizeof N.SSID, "%s", SSID);
		N.nRSSI = nRSSI;
		N.bSecure = bSecure;
	}
}

} // namespace

extern "C" boolean __real__ZN14CBcm4343Device17ReceiveScanResultEPvPj (CBcm4343Device *pThis,
									  void *pBuffer, unsigned *pLength);

extern "C" boolean __wrap__ZN14CBcm4343Device17ReceiveScanResultEPvPj (CBcm4343Device *pThis,
									  void *pBuffer, unsigned *pLength)
{
	const boolean bGot = __real__ZN14CBcm4343Device17ReceiveScanResultEPvPj (pThis, pBuffer, pLength);
	if (bGot && pBuffer != nullptr && pLength != nullptr && *pLength >= sizeof (brcmf_escan_result_le))
	{
		const auto *pResult = static_cast<const brcmf_escan_result_le *> (pBuffer);
		const u8 *pEnd = static_cast<const u8 *> (pBuffer) + *pLength;
		const brcmf_bss_info_le *pBSS = &pResult->bss_info_le;
		for (unsigned i = 0; i < pResult->bss_count; i++)
		{
			if (reinterpret_cast<const u8 *> (pBSS) + sizeof *pBSS > pEnd || pBSS->length == 0)
			{
				break;			// a short or odd buffer: stop, do not guess
			}
			NoteNetwork (pBSS);
			pBSS = reinterpret_cast<const brcmf_bss_info_le *> (reinterpret_cast<const u8 *> (pBSS) + pBSS->length);
		}
	}
	return bGot;
}

unsigned BareNetWiFiNetworks (TBareWiFiNetwork *pOut, unsigned nMax)
{
	// Strongest first.
	TBareWiFiNetwork Sorted[MaxSeen];
	memcpy (Sorted, s_Seen, sizeof Sorted);
	for (unsigned i = 1; i < s_nSeen; i++)
	{
		for (unsigned j = i; j > 0 && Sorted[j].nRSSI > Sorted[j - 1].nRSSI; j--)
		{
			const TBareWiFiNetwork t = Sorted[j]; Sorted[j] = Sorted[j - 1]; Sorted[j - 1] = t;
		}
	}
	const unsigned n = s_nSeen < nMax ? s_nSeen : nMax;
	memcpy (pOut, Sorted, n * sizeof *pOut);
	return n;
}
