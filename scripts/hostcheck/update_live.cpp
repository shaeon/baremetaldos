//
// update_live.cpp - make check-update-live (needs the network; not in the CI)
//
// The updater's HTTPS against GitHub for real: Mbed TLS 3.6.2 with BM-DOS's
// configuration, the API, the plan B, a download following GitHub's redirect
// to its file servers, and a real zip read entry by entry with its CRC.
// Adapted from ZXMetalCircle's test/host/update_live.cpp (commit 61494ca).
//
//   build/update_live sd/certs/cacert.pem     certificates checked
//   build/update_live -                       not checked (a TLS-intercepting
//                                             proxy in between; on the board
//                                             they are always checked)
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/update/httpsclient.h"
#include "../../src/update/updater.h"
#include "../../src/update/zipfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

// --- Lo que en la Pi pone Circle ---------------------------------------------

extern "C" {
int bmdos_snprintf (char *s, size_t n, const char *f, ...) { va_list a; va_start (a, f); int r = vsnprintf (s, n, f, a); va_end (a); return r; }
int bmdos_vsnprintf (char *s, size_t n, const char *f, va_list a) { return vsnprintf (s, n, f, a); }
int bmdos_printf (const char *, ...) { return 0; }
int bmdos_fprintf (FILE *, const char *, ...) { return 0; }
int mbedtls_hardware_poll (void *, unsigned char *pOut, size_t nLen, size_t *pnOut)
{
	FILE *f = fopen ("/dev/urandom", "rb");
	*pnOut = fread (pOut, 1, nLen, f);
	fclose (f);
	return 0;
}
}

class CSocket : public ITransport
{
	int m_fd = -1;
public:
	bool Connect (const char *pHost, unsigned nPort)
	{
		char Port[8]; snprintf (Port, sizeof Port, "%u", nPort);
		struct addrinfo Hints = {}, *pRes;
		Hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo (pHost, Port, &Hints, &pRes) != 0) return false;
		m_fd = socket (pRes->ai_family, pRes->ai_socktype, pRes->ai_protocol);
		bool ok = m_fd >= 0 && connect (m_fd, pRes->ai_addr, pRes->ai_addrlen) == 0;
		freeaddrinfo (pRes);
		return ok;
	}
	int Send (const unsigned char *p, size_t n) { return send (m_fd, p, n, 0); }
	int Receive (unsigned char *p, size_t n) { return recv (m_fd, p, n, 0); }
	void Close (void) { if (m_fd >= 0) close (m_fd); m_fd = -1; }
};

// --- La SD: una carpeta ------------------------------------------------------

class CDirFS : public CUpdateFS
{
public:
	std::string Root;
	std::string P (const char *p) { return Root + "/" + p; }
	boolean GetSize (const char *p, unsigned *n) { struct stat st; if (stat (P (p).c_str (), &st) || !S_ISREG (st.st_mode)) return FALSE; *n = st.st_size; return TRUE; }
	boolean Read (const char *p, u8 *b, unsigned n) { FILE *f = fopen (P (p).c_str (), "rb"); if (!f) return FALSE; bool ok = fread (b, 1, n, f) == n; fclose (f); return ok; }
	boolean Write (const char *p, const u8 *d, unsigned n) { FILE *f = fopen (P (p).c_str (), "wb"); if (!f) return FALSE; bool ok = fwrite (d, 1, n, f) == n; fclose (f); return ok; }
	boolean Remove (const char *p) { return unlink (P (p).c_str ()) == 0; }
	boolean Rename (const char *a, const char *b) { return rename (P (a).c_str (), P (b).c_str ()) == 0; }
	boolean MakeDir (const char *p) { mkdir (P (p).c_str (), 0755); struct stat st; return stat (P (p).c_str (), &st) == 0; }
};

static int s_nFails = 0;
static void Result (bool bOk, const char *pText)
{
	printf ("  %s  %s\n", bOk ? "ok    " : "FALLO ", pText);
	if (!bOk) s_nFails++;
}

int main (int argc, char **argv)
{
	static char PEM[512 * 1024];
	if (argc > 1 && strcmp (argv[1], "-") != 0)
	{
		FILE *f = fopen (argv[1], "rb");
		if (!f) { printf ("no encuentro %s\n", argv[1]); return 1; }
		size_t nPEM = fread (PEM, 1, sizeof PEM - 1, f);
		fclose (f);
		PEM[nPEM] = '\0';
		HTTPSSetRootCerts (PEM, nPEM + 1);
		printf ("certificados: %s (%zu bytes)\n", argv[1], nPEM);
	}
	else
	{
		HTTPSSetRootCerts (0, 0);
		printf ("certificados: NO se comprueban (-)\n");
	}

	CSocket Sock;
	CHTTPSClient Client (&Sock);
	static unsigned char Small[1024 * 1024];
	size_t nLen = 0;
	char Version[16] = "", URL[512] = "";

	// 1. GitHub's API for the default repository
	int nStatus = Client.Get ("https://api.github.com/repos/shaeon/baremetaldos/releases/latest",
				  Small, sizeof Small, &nLen);
	const bool bAPI = nStatus == 200 && CUpdater::ParseRelease ((const char *) Small, nLen, Version, sizeof Version, URL, sizeof URL);
	printf ("API: código %d, %s\n", nStatus, bAPI ? Version
		: nStatus == 404 ? "ninguna release publicada todavía"
		: nStatus < 0 ? Client.GetError () : "sin versión (¿límite de consultas?)");
	Result (nStatus > 0, "TLS con api.github.com y una respuesta HTTP");

	// 2. The plan B: where releases/latest redirects to
	static char Loc[1024];
	nStatus = Client.GetRedirect ("https://github.com/shaeon/baremetaldos/releases/latest", Loc, sizeof Loc);
	char V2[16] = "", U2[512] = "";
	const bool bB = CUpdater::ParseTagURL (Loc, V2, sizeof V2, U2, sizeof U2);
	printf ("plan B: código %d -> %s (%s)\n", nStatus, Loc, bB ? V2 : "sin release");
	Result (nStatus == 302 || nStatus == 301, "TLS con github.com y su redirección");
	Result (bAPI == bB && (!bAPI || strcmp (Version, V2) == 0), "la API y el plan B dicen lo mismo");

	// 3. A release file, through GitHub's redirect to its file servers
	nStatus = Client.Get ("https://github.com/mmoroca/baremetaldos/releases/download/v0.80/bm-dos-2026-11-11.80.img.zip.sha256",
			      Small, sizeof Small - 1, &nLen);
	Small[nLen] = 0;
	printf ("fichero de release: código %d, %zu bytes, final en %s\n", nStatus, nLen, Client.GetFinalURL ());
	Result (nStatus == 200 && nLen >= 64 && strstr ((const char *) Small, "bm-dos-") != 0,
		"descarga siguiendo la redirección al servidor de ficheros");

	// 4. A real zip (the repository at v0.78, from codeload): every entry
	// extracted and its CRC checked, as the installer does with the package.
	const size_t nBig = 64 * 1024 * 1024;
	unsigned char *pZip = new unsigned char[nBig];
	time_t t0 = time (0);
	nStatus = Client.Get ("https://github.com/shaeon/baremetaldos/archive/refs/tags/v0.78.zip", pZip, nBig, &nLen);
	printf ("zip real: código %d, %zu KB en %ld s\n", nStatus, nLen / 1024, (long) (time (0) - t0));
	CZipFile Zip (4096);
	unsigned nGood = 0, nFiles = 0;
	if (nStatus == 200 && Zip.Open (pZip, nLen))
	{
		for (unsigned i = 0; i < Zip.GetCount (); i++)
		{
			if (Zip.IsDirectory (i)) continue;
			nFiles++;
			std::vector<u8> Out (Zip.GetSize (i) + 1);
			if (Zip.Extract (i, Out.data (), Out.size ()) == (int) Zip.GetSize (i)) nGood++;
		}
	}
	printf ("  %u de %u entradas extraídas con su CRC bien\n", nGood, nFiles);
	Result (nFiles > 50 && nGood == nFiles, "un zip real de GitHub, entero y con todos los CRC");
	delete [] pZip;

	printf ("\n%d fallos\n", s_nFails);
	return s_nFails ? 1 : 0;
}
