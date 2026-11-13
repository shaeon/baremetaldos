//
// bare_update.cpp
//
// BM-DOS - the updater: GitHub's latest release, downloaded and installed from
// the setup menu (Win+F12, Sistema, Actualización).  The pieces are
// ZXMetalCircle's (src/update/: the install rules, the zip, HTTPS with Mbed
// TLS); this file is the flow, after ZXMetalCircle's kernelupdate.cpp (commit
// 61494ca), rewritten for this port's menu:
//
//   * which version is the latest: the web's redirect first (releases/latest
//     -> .../tag/v0.81: quick, and not counted against the API's limit of 60
//     queries an hour), GitHub's API if that says nothing;
//   * at boot, when boot.conf asks (update_check = on), in a task of its own,
//     so nothing waits for GitHub: it only says so - installing is always
//     done from the menu;
//   * download into memory, keep the package in update/ (to install again,
//     or go back), install it (CUpdater: CRCs first, only what differs,
//     .new and rename, the kernels last, update/EN_CURSO while it runs),
//     prune the old packages;
//   * HTTPS is checked against SD:/certs/cacert.pem.  Without that file
//     nothing is downloaded: the check is what stands between the board and
//     a forged kernel.
//
// Core 0, from the menu (the frame loop is paused while it runs) or from the
// boot check's task.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"
#include "../update/updater.h"
#include "../update/updatefs.h"
#include "../update/httpsclient.h"
#include "../update/circleglue.h"

#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

// The version this kernel is (Makefile, build/bmdos_version.h).
#if __has_include("../../build/bmdos_version.h")
#include "../../build/bmdos_version.h"
#endif
#ifndef BMDOS_VERSION
#define BMDOS_VERSION "0.00"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char FromUpdate[] = "update";

constexpr size_t PageMax = 1024 * 1024;		// GitHub's API answer
constexpr size_t PackageMax = 160 * 1024 * 1024;	// the card's zip: room to grow

char s_Repo[64] = "shaeon/baremetaldos";

// What is known about GitHub's latest release.
char s_Latest[16] = "";
char s_LatestURL[512] = "";
bool s_bChecked = false;
char s_Error[128] = "";

// update/EN_CURSO at boot: an install that did not finish (its version).
char s_Interrupted[16] = "";

// The boot check: its task writes these, the menu reads them.
volatile bool s_bBootCheckRunning = false;

// --- the root certificates ---------------------------------------------------

bool s_bCertsLoaded = false;

bool LoadCerts (void)
{
	if (s_bCertsLoaded)
	{
		return true;
	}
	FILE *pFile = fopen ("SD:/certs/cacert.pem", "rb");
	if (pFile == nullptr)
	{
		snprintf (s_Error, sizeof s_Error, "falta certs/cacert.pem en la tarjeta");
		return false;
	}
	fseek (pFile, 0, SEEK_END);
	const long nSize = ftell (pFile);
	fseek (pFile, 0, SEEK_SET);
	// Kept for the rest of the run: Mbed TLS parses it, the PEM text is not
	// needed afterwards, but parsing happens once.
	char *pPEM = nSize > 0 ? static_cast<char *> (malloc (static_cast<size_t> (nSize) + 1)) : nullptr;
	const bool bRead = pPEM != nullptr && fread (pPEM, 1, static_cast<size_t> (nSize), pFile) == static_cast<size_t> (nSize);
	fclose (pFile);
	if (!bRead)
	{
		free (pPEM);
		snprintf (s_Error, sizeof s_Error, "no se pudo leer certs/cacert.pem");
		return false;
	}
	pPEM[nSize] = '\0';
	HTTPSSetRootCerts (pPEM, static_cast<size_t> (nSize) + 1);
	free (pPEM);
	s_bCertsLoaded = true;
	return true;
}

// --- a transport that says what it is doing ------------------------------------
//
// Connecting and negotiating TLS is the slow part on a Pi: the message says
// with whom and for how long, so that it does not look hung (ZXMetalCircle's
// CBusyTransport).  Quiet in the boot check, which shows nothing.

class CShowingTransport : public ITransport
{
public:
	explicit CShowingTransport (bool bShow) : m_bShow (bShow) {}

	bool Connect (const char *pHost, unsigned nPort) override
	{
		if (m_bShow)
		{
			std::string Text = std::string ("Conectando con ") + pHost + "...";
			BareMenuShowMessage (Text.c_str ());
		}
		return m_Inner.Connect (pHost, nPort);
	}
	int Send (const unsigned char *pData, size_t nLength) override	{ return m_Inner.Send (pData, nLength); }
	int Receive (unsigned char *pBuffer, size_t nSize) override	{ return m_Inner.Receive (pBuffer, nSize); }
	void Close (void) override					{ m_Inner.Close (); }

private:
	CCircleTransport m_Inner;
	bool m_bShow;
};

// --- which version is the latest ------------------------------------------------

bool QueryLatest (ITransport *pTransport, char *pVersion, size_t nVersionSize, char *pURL, size_t nURLSize)
{
	CHTTPSClient Client (pTransport);
	const std::string Web = std::string ("https://github.com/") + s_Repo + "/releases/latest";
	const std::string API = std::string ("https://api.github.com/repos/") + s_Repo + "/releases/latest";

	bool bFound = false;
	char *pLocation = new char[1024];
	if (pLocation != nullptr)
	{
		const int nStatus = Client.GetRedirect (Web.c_str (), pLocation, 1024);
		if (nStatus >= 300 && nStatus < 400)
		{
			bFound = CUpdater::ParseTagURL (pLocation, pVersion, nVersionSize, pURL, nURLSize);
		}
		else if (nStatus < 0)
		{
			snprintf (s_Error, sizeof s_Error, "%s", Client.GetError ());
		}
		delete [] pLocation;
	}
	if (!bFound)
	{
		u8 *pPage = new u8[PageMax];
		size_t nLength = 0;
		if (pPage != nullptr && Client.Get (API.c_str (), pPage, PageMax, &nLength) == 200)
		{
			bFound = CUpdater::ParseRelease (reinterpret_cast<const char *> (pPage), nLength,
							 pVersion, nVersionSize, pURL, nURLSize);
		}
		delete [] pPage;
	}
	return bFound;
}

// After a query: the result where the menu reads it.
void Remember (bool bFound, const char *pVersion, const char *pURL)
{
	s_bChecked = true;
	if (bFound)
	{
		snprintf (s_Latest, sizeof s_Latest, "%s", pVersion);
		snprintf (s_LatestURL, sizeof s_LatestURL, "%s", pURL);
		s_Error[0] = '\0';
	}
	else if (s_Error[0] == '\0')
	{
		snprintf (s_Error, sizeof s_Error, "no hay ninguna release publicada en %s", s_Repo);
	}
	CLogger::Get ()->Write (FromUpdate, LogNotice, "latest release of %s: %s", s_Repo,
				bFound ? pVersion : s_Error);
}

// The boot check, in a task of its own: the machine goes on meanwhile.
class CBootCheckTask : public CTask
{
public:
	CBootCheckTask (void) : CTask (0x20000) {}	// TLS needs a deep stack

	void Run (void) override
	{
		// Wait for the network (it starts at boot; a minute is plenty).
		for (unsigned i = 0; i < 120 && !BareNetSocketsReady (); i++)
		{
			CScheduler::Get ()->MsSleep (500);
		}
		char Version[16] = "", URL[512] = "";
		bool bFound = false;
		if (BareNetSocketsReady () && LoadCerts ())
		{
			CShowingTransport Transport (false);
			bFound = QueryLatest (&Transport, Version, sizeof Version, URL, sizeof URL);
		}
		else if (s_Error[0] == '\0')
		{
			snprintf (s_Error, sizeof s_Error, "sin red");
		}
		Remember (bFound, Version, URL);
		s_bBootCheckRunning = false;
	}
};

// --- download progress ------------------------------------------------------------

struct TProgressState
{
	const char *pVersion;
	unsigned nLastTicks;
};

void DownloadProgress (void *pParam, size_t nReceived, size_t nTotal)
{
	TProgressState *pState = static_cast<TProgressState *> (pParam);
	const unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - pState->nLastTicks < 250000 && nReceived != nTotal)
	{
		return;
	}
	pState->nLastTicks = nNow;
	char Text[96];
	if (nTotal != 0)
	{
		snprintf (Text, sizeof Text, "Descargando BM-DOS %s: %u de %u KB", pState->pVersion,
			  static_cast<unsigned> (nReceived / 1024), static_cast<unsigned> (nTotal / 1024));
	}
	else
	{
		snprintf (Text, sizeof Text, "Descargando BM-DOS %s: %u KB", pState->pVersion,
			  static_cast<unsigned> (nReceived / 1024));
	}
	BareMenuShowMessage (Text);
}

void InstallProgress (void *pParam, unsigned nDone, unsigned nTotal)
{
	TProgressState *pState = static_cast<TProgressState *> (pParam);
	const unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - pState->nLastTicks < 250000 && nDone != nTotal)
	{
		return;
	}
	pState->nLastTicks = nNow;
	char Text[96];
	snprintf (Text, sizeof Text, "Instalando BM-DOS %s: %u de %u", pState->pVersion, nDone, nTotal);
	BareMenuShowMessage (Text);
}

// --- the packages kept in update/ -------------------------------------------------

// Versions of the packages in update/, newest first.
unsigned ScanPackages (char (*pVersions)[16], unsigned nMax)
{
	unsigned n = 0;
	DIR Dir;
	FILINFO Info;
	if (f_opendir (&Dir, "SD:/" UPDATE_DIR) != FR_OK)
	{
		return 0;
	}
	while (f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
	{
		const size_t nLen = strlen (Info.fname);
		// BM-DOS-<version>-sd.zip
		if ((Info.fattrib & AM_DIR) || nLen <= 14 || nLen - 14 >= 16
		    || strncasecmp (Info.fname, "BM-DOS-", 7) != 0
		    || strcasecmp (Info.fname + nLen - 7, "-sd.zip") != 0 || n >= nMax)
		{
			continue;
		}
		char Version[16];
		memcpy (Version, Info.fname + 7, nLen - 14);
		Version[nLen - 14] = '\0';
		unsigned i = n++;
		while (i > 0 && CUpdater::CompareVersions (pVersions[i - 1], Version) < 0)
		{
			strcpy (pVersions[i], pVersions[i - 1]);
			i--;
		}
		strcpy (pVersions[i], Version);
	}
	f_closedir (&Dir);
	return n;
}

// Keep the installed package and the one before it, unless all are wanted.
void Prune (const char *pInstalled, bool bKeepAll)
{
	if (bKeepAll)
	{
		return;
	}
	char Versions[32][16];
	const unsigned n = ScanPackages (Versions, 32);
	// The installed one, and the newest one older than it.
	bool bPreviousKept = false;
	for (unsigned i = 0; i < n; i++)
	{
		const int nCmp = CUpdater::CompareVersions (Versions[i], pInstalled);
		bool bKeep = nCmp == 0;
		if (nCmp < 0 && !bPreviousKept)
		{
			bKeep = bPreviousKept = true;
		}
		if (!bKeep)
		{
			char Name[64];
			CUpdater::PackageName (Versions[i], Name, sizeof Name);
			const std::string Path = std::string ("SD:/") + Name;
			const FRESULT Result = f_unlink (Path.c_str ());
			CLogger::Get ()->Write (FromUpdate, LogNotice, "pruned %s: %s", Name,
						Result == FR_OK ? "ok" : "failed");
		}
	}
}

u8 *ReadPackage (const char *pVersion, unsigned *pSize)
{
	char Name[64];
	CUpdater::PackageName (pVersion, Name, sizeof Name);
	CFatUpdateFS FS;
	unsigned nSize = 0;
	if (!FS.GetSize (Name, &nSize) || nSize == 0)
	{
		return nullptr;
	}
	u8 *pZip = static_cast<u8 *> (malloc (nSize));
	if (pZip != nullptr && !FS.Read (Name, pZip, nSize))
	{
		free (pZip);
		return nullptr;
	}
	*pSize = nSize;
	return pZip;
}

} // namespace

// --- the interface (bare_env.h) -----------------------------------------------------

void BareUpdateInit (const char *pRepo, boolean bBootCheck)
{
	if (pRepo != nullptr && pRepo[0] != '\0')
	{
		snprintf (s_Repo, sizeof s_Repo, "%s", pRepo);
	}

	// An install that did not finish leaves update/EN_CURSO with its version.
	CFatUpdateFS FS;
	unsigned nSize = 0;
	if (FS.GetSize (UPDATE_MARKER, &nSize) && nSize > 0 && nSize < sizeof s_Interrupted)
	{
		FS.Read (UPDATE_MARKER, reinterpret_cast<u8 *> (s_Interrupted), nSize);
		s_Interrupted[nSize] = '\0';
		CLogger::Get ()->Write (FromUpdate, LogWarning,
					"the update to %s did not finish: install it again (Win+F12, Sistema)",
					s_Interrupted);
	}

	if (bBootCheck)
	{
		s_bBootCheckRunning = true;
		new CBootCheckTask;		// it deletes itself when it ends
	}
}

void BareUpdateGetInfo (TBareUpdateInfo *pInfo)
{
	pInfo->pRepo = s_Repo;
	pInfo->bChecking = s_bBootCheckRunning;
	pInfo->bChecked = s_bChecked;
	pInfo->pLatest = s_Latest;
	pInfo->bNewer = s_Latest[0] != '\0' && CUpdater::CompareVersions (s_Latest, BareUpdateInstalledVersion ()) > 0;
	pInfo->pError = s_Error;
	pInfo->pInterrupted = s_Interrupted;
}

boolean BareUpdateCheck (void)
{
	s_Error[0] = '\0';
	if (!BareNetSocketsReady ())
	{
		snprintf (s_Error, sizeof s_Error, "la red no está conectada (Red)");
		s_bChecked = true;
		return FALSE;
	}
	if (!LoadCerts ())
	{
		s_bChecked = true;
		return FALSE;
	}
	char Version[16] = "", URL[512] = "";
	CShowingTransport Transport (true);
	const bool bFound = QueryLatest (&Transport, Version, sizeof Version, URL, sizeof URL);
	Remember (bFound, Version, URL);
	return bFound ? TRUE : FALSE;
}

unsigned BareUpdatePackages (char (*pVersions)[16], unsigned nMax)
{
	return ScanPackages (pVersions, nMax);
}

boolean BareUpdateInstall (const char *pVersion, boolean bReplaceConfig, boolean bKeepAll,
			   char *pSummary, unsigned nSummarySize)
{
	s_Error[0] = '\0';
	TProgressState State { pVersion, 0 };
	u8 *pZip = nullptr;
	unsigned nZipSize = 0;

	// A package kept in update/ is installed from there; otherwise it is
	// downloaded - the latest release's.
	pZip = ReadPackage (pVersion, &nZipSize);
	if (pZip == nullptr)
	{
		if (strcmp (pVersion, s_Latest) != 0 || s_LatestURL[0] == '\0')
		{
			snprintf (pSummary, nSummarySize, "No está el paquete de la %s en update/.", pVersion);
			return FALSE;
		}
		if (!BareNetSocketsReady () || !LoadCerts ())
		{
			snprintf (pSummary, nSummarySize, "No se puede descargar: %s.",
				  s_Error[0] != '\0' ? s_Error : "la red no está conectada");
			return FALSE;
		}

		// Room: what malloc's heap has, less a margin for the rest; the
		// card's zip is tens of MB.
		const size_t nFree = CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW);
		size_t nRoom = nFree > 48u * 1024 * 1024 ? nFree - 48u * 1024 * 1024 : 0;
		if (nFree == 0 || nRoom > PackageMax) nRoom = PackageMax;
		u8 *pBuffer = nRoom >= 16u * 1024 * 1024 ? static_cast<u8 *> (malloc (nRoom)) : nullptr;
		if (pBuffer == nullptr)
		{
			snprintf (pSummary, nSummarySize, "No hay memoria para descargar el paquete.");
			return FALSE;
		}

		CShowingTransport Transport (true);
		CHTTPSClient Client (&Transport);
		Client.SetProgress (DownloadProgress, &State);
		size_t nLength = 0;
		const int nStatus = Client.Get (s_LatestURL, pBuffer, nRoom, &nLength);
		Client.SetProgress (nullptr, nullptr);
		if (nStatus != 200 || nLength == 0)
		{
			snprintf (pSummary, nSummarySize, "La descarga falló (%d): %s", nStatus, Client.GetError ());
			free (pBuffer);
			return FALSE;
		}
		pZip = pBuffer;
		nZipSize = static_cast<unsigned> (nLength);

		// Kept in update/: to install again, or to go back to later.
		CFatUpdateFS FS;
		char Name[64];
		CUpdater::PackageName (pVersion, Name, sizeof Name);
		FS.MakeDir (UPDATE_DIR);
		if (!FS.Write (Name, pZip, nZipSize))
		{
			CLogger::Get ()->Write (FromUpdate, LogWarning, "could not keep %s", Name);
		}
	}

	BareMenuShowMessage ("Comprobando el paquete...");
	CFatUpdateFS FS;
	TUpdateResult Result;
	State.nLastTicks = 0;
	CUpdater::Install (&FS, pZip, nZipSize, bReplaceConfig, pVersion, &Result, InstallProgress, &State);
	free (pZip);

	switch (Result.Status)
	{
	case UpdateOK:
		s_Interrupted[0] = '\0';
		Prune (pVersion, bKeepAll);
		if (Result.nNotes != 0)
		{
			snprintf (pSummary, nSummarySize,
				  "BM-DOS %s instalado: %u ficheros cambiados. %u de configuración "
				  "nuevos quedan como .nuevo (los tuyos no se han tocado).",
				  pVersion, Result.nChanged, Result.nNotes);
		}
		else
		{
			snprintf (pSummary, nSummarySize, "BM-DOS %s instalado: %u ficheros cambiados.",
				  pVersion, Result.nChanged);
		}
		CLogger::Get ()->Write (FromUpdate, LogNotice, "installed %s: %u changed, %u unchanged, %u notes",
					pVersion, Result.nChanged, Result.nUnchanged, Result.nNotes);
		return TRUE;

	case UpdateBadPackage:
		snprintf (pSummary, nSummarySize, "El paquete de la %s está dañado: no se ha tocado nada.", pVersion);
		return FALSE;

	case UpdateNoMemory:
		snprintf (pSummary, nSummarySize, "No hay memoria para instalar: no se ha tocado nada.");
		return FALSE;

	case UpdateWriteError:
	default:
		snprintf (s_Interrupted, sizeof s_Interrupted, "%s", pVersion);
		snprintf (pSummary, nSummarySize,
			  "Falló al escribir %s: la instalación queda a medias. Vuelve a instalar la %s "
			  "antes de reiniciar.", Result.Failed, pVersion);
		return FALSE;
	}
}

const char *BareUpdateInstalledVersion (void)
{
	return BMDOS_VERSION;
}
