//
// update_test.cpp - make check-update
//
// The updater imported from ZXMetalCircle (src/update/), with BM-DOS's
// files: its install rules on a "card" that is a folder, and a zip with real
// deflate made by Python (Makefile) checked entry by entry.  Adapted from
// ZXMetalCircle's test/host/update_test.cpp (commit 61494ca): same machinery,
// the scenarios rewritten for BM-DOS - dos/ never touched, the five
// configuration files kept, wpa_supplicant.conf among them.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/update/updater.h"
#include "../../src/update/zipfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
static void chk (const char *pWhat, bool bOK)
{
	printf ("%s  %s\n", bOK ? "ok   " : "FALLO", pWhat);
	if (!bOK) fails++;
}

// --- La "SD": una carpeta, contando lo que se escribe ---------------------

class CDirFS : public CUpdateFS
{
public:
	std::string Root;
	std::vector<std::string> Written;	// en orden
	std::string FailOn;			// escribir este fichero falla

	std::string P (const char *pPath) { return Root + "/" + pPath; }
	boolean GetSize (const char *pPath, unsigned *pSize)
	{
		struct stat st;
		if (stat (P (pPath).c_str (), &st) != 0 || !S_ISREG (st.st_mode)) return FALSE;
		*pSize = st.st_size;
		return TRUE;
	}
	boolean Read (const char *pPath, u8 *pBuffer, unsigned nSize)
	{
		FILE *f = fopen (P (pPath).c_str (), "rb");
		if (!f) return FALSE;
		bool ok = fread (pBuffer, 1, nSize, f) == nSize;
		fclose (f);
		return ok;
	}
	boolean Write (const char *pPath, const u8 *pData, unsigned nSize)
	{
		std::string Name (pPath);
		if (!FailOn.empty () && Name == FailOn + ".new") return FALSE;
		FILE *f = fopen (P (pPath).c_str (), "wb");
		if (!f) return FALSE;
		bool ok = fwrite (pData, 1, nSize, f) == nSize;
		fclose (f);
		if (Name.size () > 4 && Name.substr (Name.size () - 4) == ".new")
			Written.push_back (Name.substr (0, Name.size () - 4));
		return ok;
	}
	boolean Remove (const char *pPath) { return unlink (P (pPath).c_str ()) == 0; }
	boolean Rename (const char *pFrom, const char *pTo)
	{
		unsigned n;
		if (GetSize (pTo, &n)) return FALSE;		// como FatFs
		return rename (P (pFrom).c_str (), P (pTo).c_str ()) == 0;
	}
	boolean MakeDir (const char *pPath)
	{
		mkdir (P (pPath).c_str (), 0755);
		struct stat st;
		return stat (P (pPath).c_str (), &st) == 0 && S_ISDIR (st.st_mode);
	}
};

static std::string ReadText (CDirFS &fs, const char *pPath)
{
	unsigned n;
	if (!fs.GetSize (pPath, &n)) return "(no existe)";
	std::string s (n, '\0');
	fs.Read (pPath, (u8 *) &s[0], n);
	return s;
}

static void WriteText (CDirFS &fs, const char *pPath, const char *pText)
{
	FILE *f = fopen (fs.P (pPath).c_str (), "wb");
	fputs (pText, f);
	fclose (f);
}

// --- Zips sin comprimir -----------------------------------------------------

typedef std::vector<std::pair<std::string, std::string> > TFiles;

static void Put16 (std::string &s, unsigned v) { s += (char) v; s += (char) (v >> 8); }
static void Put32 (std::string &s, unsigned v) { Put16 (s, v & 0xFFFF); Put16 (s, v >> 16); }

static std::string MakeZip (const TFiles &Files, bool bCorrupt = false)
{
	std::string Zip, Dir;
	for (size_t i = 0; i < Files.size (); i++)
	{
		const std::string &Name = Files[i].first, &Data = Files[i].second;
		u32 nCRC = CZipFile::CRC32 ((const u8 *) Data.data (), Data.size ());
		if (bCorrupt && i == Files.size () - 1) nCRC ^= 1;
		unsigned nOffset = Zip.size ();
		Put32 (Zip, 0x04034B50); Put16 (Zip, 10); Put16 (Zip, 0); Put16 (Zip, 0);
		Put32 (Zip, 0); Put32 (Zip, nCRC); Put32 (Zip, Data.size ()); Put32 (Zip, Data.size ());
		Put16 (Zip, Name.size ()); Put16 (Zip, 0);
		Zip += Name; Zip += Data;
		Put32 (Dir, 0x02014B50); Put16 (Dir, 20); Put16 (Dir, 10); Put16 (Dir, 0); Put16 (Dir, 0);
		Put32 (Dir, 0); Put32 (Dir, nCRC); Put32 (Dir, Data.size ()); Put32 (Dir, Data.size ());
		Put16 (Dir, Name.size ()); Put16 (Dir, 0); Put16 (Dir, 0); Put16 (Dir, 0); Put16 (Dir, 0);
		Put32 (Dir, 0); Put32 (Dir, nOffset);
		Dir += Name;
	}
	unsigned nDirOffset = Zip.size ();
	Zip += Dir;
	Put32 (Zip, 0x06054B50); Put16 (Zip, 0); Put16 (Zip, 0);
	Put16 (Zip, Files.size ()); Put16 (Zip, Files.size ());
	Put32 (Zip, Dir.size ()); Put32 (Zip, nDirOffset); Put16 (Zip, 0);
	return Zip;
}

static TUpdateResult Install (CDirFS &fs, const std::string &Zip, bool bReplace, const char *pVersion)
{
	TUpdateResult R;
	fs.Written.clear ();
	CUpdater::Install (&fs, (const u8 *) Zip.data (), Zip.size (), bReplace, pVersion, &R);
	return R;
}

static bool WasWritten (CDirFS &fs, const char *pPath)
{
	for (size_t i = 0; i < fs.Written.size (); i++)
		if (fs.Written[i] == pPath) return true;
	return false;
}

int main (int argc, char **argv)
{
	char Tmp[] = "/tmp/bmdosupdXXXXXX";
	CDirFS fs;
	fs.Root = mkdtemp (Tmp);

	TFiles V1 = {
		{"kernel8-rpi4.img", "kernel 1"}, {"kernel_2712.img", "kernel5 1"},
		{"boot.conf", "network = wifi"}, {"dosbox.conf", "[sblaster]"},
		{"cmdline.txt", "keymap=es"}, {"config.txt", "arm_64bit=1"},
		{"wpa_supplicant.conf", "country=ES"},
		{"start4.elf", "firmware 1"}, {"README.md", "leeme 1"},
		{"dos/AUTOEXEC.BAT", "@echo off"}, {"dos/README.TXT", "pon aqui tus juegos"},
	};
	std::string Zip1 = MakeZip (V1);

	// 1. A card with only the user's C: (dos/): everything else installed
	mkdir (fs.P ("dos").c_str (), 0755);
	WriteText (fs, "dos/AUTOEXEC.BAT", "C:\\GAMES\\DOOM\\DOOM.EXE");
	TUpdateResult R = Install (fs, Zip1, false, "1.00");
	chk ("instalar: bien", R.Status == UpdateOK);
	chk ("instalar: ficheros y configuración escritos",
	     ReadText (fs, "start4.elf") == "firmware 1" && ReadText (fs, "boot.conf") == "network = wifi");
	chk ("dos/: el AUTOEXEC.BAT del usuario no se toca", ReadText (fs, "dos/AUTOEXEC.BAT") == "C:\\GAMES\\DOOM\\DOOM.EXE");
	chk ("dos/: tampoco se añade nada", ReadText (fs, "dos/README.TXT") == "(no existe)");
	chk ("instalar: no queda la marca update/EN_CURSO", ReadText (fs, UPDATE_MARKER) == "(no existe)");
	chk ("instalar: los kernels, los últimos",
	     fs.Written.size () >= 3 && CUpdater::IsKernel (fs.Written[fs.Written.size () - 1].c_str ())
	     && CUpdater::IsKernel (fs.Written[fs.Written.size () - 2].c_str ())
	     && !CUpdater::IsKernel (fs.Written[fs.Written.size () - 3].c_str ()));
	chk ("kernel8-rpi4.img y kernel_2712.img cuentan como kernels",
	     CUpdater::IsKernel ("kernel8-rpi4.img") && CUpdater::IsKernel ("kernel_2712.img")
	     && !CUpdater::IsKernel ("dos/kernel.img"));

	// 2. The same package again: nothing to do
	R = Install (fs, Zip1, false, "1.00");
	chk ("mismo paquete: nada cambiado", R.Status == UpdateOK && R.nChanged == 0 && fs.Written.empty ());

	// 3. The user changes his settings; the next version changes several things
	WriteText (fs, "boot.conf", "network = ethernet");
	WriteText (fs, "wpa_supplicant.conf", "country=ES\nnetwork={ssid=\"casa\" psk=\"secreta\"}");
	TFiles V2 = V1;
	V2[0].second = "kernel 2";				// kernel8-rpi4.img
	V2[2].second = "network = wifi\nweb_server = off";	// boot.conf, new
	V2[6].second = "country=ES\n# plantilla";		// wpa_supplicant.conf, new
	V2[7].second = "firmware 2";				// start4.elf
	V2[9].second = "@echo on";				// dos/AUTOEXEC.BAT
	V2.push_back ({"certs/ca.pem", "-----BEGIN"});		// a new file
	std::string Zip2 = MakeZip (V2);

	R = Install (fs, Zip2, false, "1.01");
	chk ("1.01 conservando la configuración: bien", R.Status == UpdateOK);
	chk ("boot.conf del usuario intacto y el nuevo en .nuevo",
	     ReadText (fs, "boot.conf") == "network = ethernet"
	     && ReadText (fs, "boot.conf.nuevo") == "network = wifi\nweb_server = off");
	chk ("wpa_supplicant.conf (la contraseña del WiFi) intacto",
	     ReadText (fs, "wpa_supplicant.conf") == "country=ES\nnetwork={ssid=\"casa\" psk=\"secreta\"}");
	chk ("dosbox.conf, igual que el nuevo: ni se toca ni .nuevo",
	     !WasWritten (fs, "dosbox.conf") && ReadText (fs, "dosbox.conf.nuevo") == "(no existe)");
	chk ("se avisa de los dos .nuevo", R.nNotes == 2);
	chk ("firmware y kernel cambiados se copian",
	     ReadText (fs, "start4.elf") == "firmware 2" && ReadText (fs, "kernel8-rpi4.img") == "kernel 2");
	chk ("lo que no cambia no se escribe",
	     !WasWritten (fs, "README.md") && !WasWritten (fs, "kernel_2712.img"));
	chk ("fichero nuevo del paquete, copiado (con su carpeta)", ReadText (fs, "certs/ca.pem") == "-----BEGIN");
	chk ("dos/: tampoco con un paquete que lo cambia", ReadText (fs, "dos/AUTOEXEC.BAT") == "C:\\GAMES\\DOOM\\DOOM.EXE");
	chk ("cambiados: 3 (kernel, firmware, ca.pem); los .nuevo no cuentan", R.nChanged == 3);

	R = Install (fs, Zip2, false, "1.01");
	chk ("reinstalar la 1.01: nada cambiado ni escrito, sin avisos",
	     R.Status == UpdateOK && R.nChanged == 0 && fs.Written.empty () && R.nNotes == 0);

	// 4. Installing it again, taking the new settings
	R = Install (fs, Zip2, true, "1.01");
	chk ("poniendo la configuración nueva: bien", R.Status == UpdateOK);
	chk ("boot.conf pasa a ser el nuevo y el del usuario queda en .bak",
	     ReadText (fs, "boot.conf") == "network = wifi\nweb_server = off"
	     && ReadText (fs, "boot.conf.bak") == "network = ethernet");
	chk ("dos/: ni con la configuración nueva", ReadText (fs, "dos/AUTOEXEC.BAT") == "C:\\GAMES\\DOOM\\DOOM.EXE");

	// 5. Back to 1.00 (a kept package)
	R = Install (fs, Zip1, false, "1.00");
	chk ("vuelta a la 1.00: firmware y kernel vuelven",
	     R.Status == UpdateOK && ReadText (fs, "start4.elf") == "firmware 1" && ReadText (fs, "kernel8-rpi4.img") == "kernel 1");

	// 6. A damaged package: nothing touched
	std::string Bad = MakeZip (V2, true);
	R = Install (fs, Bad, false, "1.01");
	chk ("paquete dañado: se rechaza sin escribir nada ni dejar marca",
	     R.Status == UpdateBadPackage && fs.Written.empty () && ReadText (fs, UPDATE_MARKER) == "(no existe)");

	// 7. A write fails half way: it stops, the marker stays, the kernels are untouched
	fs.FailOn = "start4.elf";
	R = Install (fs, Zip2, false, "1.01");
	chk ("error al escribir: se informa del fichero", R.Status == UpdateWriteError && strcmp (R.Failed, "start4.elf") == 0);
	chk ("error al escribir: queda update/EN_CURSO con la versión", ReadText (fs, UPDATE_MARKER) == "1.01");
	chk ("error al escribir: los kernels no se han tocado", ReadText (fs, "kernel8-rpi4.img") == "kernel 1");
	fs.FailOn = "";
	R = Install (fs, Zip2, false, "1.01");
	chk ("reintento: termina bien y quita la marca",
	     R.Status == UpdateOK && ReadText (fs, UPDATE_MARKER) == "(no existe)" && ReadText (fs, "kernel8-rpi4.img") == "kernel 2");

	// 8. Versions
	chk ("versiones: 1.00 < 1.01, 1.10 > 1.9, 1.01 = 1.01",
	     CUpdater::CompareVersions ("1.00", "1.01") < 0 && CUpdater::CompareVersions ("1.10", "1.9") > 0
	     && CUpdater::CompareVersions ("1.01", "1.01") == 0);

	// 9. GitHub's answer (its API's format), the plan B, the kept name
	{
		const char *JSON =
			"{\"url\": \"https://api.github.com/repos/shaeon/baremetaldos/releases/1\",\n"
			" \"tag_name\": \"v1.01\", \"name\": \"BM-DOS 1.01\", \"draft\": false,\n"
			" \"assets\": [ {\"name\": \"kernel8-rpi4.img\", \"browser_download_url\": "
			"\"https://github.com/shaeon/baremetaldos/releases/download/v1.01/kernel8-rpi4.img\"},\n"
			"  {\"name\": \"BM-DOS-card.zip\", \"size\": 25000000,\n"
			"   \"browser_download_url\": \"https://github.com/shaeon/baremetaldos/releases/download/v1.01/BM-DOS-card.zip\"} ]}";
		char V[16], U[256];
		bool bOK = CUpdater::ParseRelease (JSON, strlen (JSON), V, sizeof V, U, sizeof U);
		chk ("API: versión 1.01 y el zip de la tarjeta (no un kernel suelto)",
		     bOK && strcmp (V, "1.01") == 0
		     && strcmp (U, "https://github.com/shaeon/baremetaldos/releases/download/v1.01/BM-DOS-card.zip") == 0);
		const char *Limit = "{\"message\":\"API rate limit exceeded for 1.2.3.4.\"}";
		chk ("API: límite de consultas -> no hay versión", !CUpdater::ParseRelease (Limit, strlen (Limit), V, sizeof V, U, sizeof U));
		bOK = CUpdater::ParseTagURL ("https://github.com/shaeon/baremetaldos/releases/tag/v1.01", V, sizeof V, U, sizeof U);
		chk ("plan B: de .../releases/tag/v1.01, la versión y el zip",
		     bOK && strcmp (V, "1.01") == 0
		     && strcmp (U, "https://github.com/shaeon/baremetaldos/releases/download/v1.01/BM-DOS-card.zip") == 0);
		char N[64];
		CUpdater::PackageName ("1.01", N, sizeof N);
		chk ("nombre guardado: update/BM-DOS-1.01-card.zip", strcmp (N, "update/BM-DOS-1.01-card.zip") == 0);
	}

	// 10. Real deflate: a zip made by Python's zipfile (argv[1]) and the
	// expected contents (argv[2], a folder), entry by entry.
	if (argc > 2)
	{
		FILE *f = fopen (argv[1], "rb");
		std::string Z; char Buf[65536]; size_t n;
		while (f && (n = fread (Buf, 1, sizeof Buf, f)) > 0) Z.append (Buf, n);
		if (f) fclose (f);
		CZipFile Zip (64);
		bool bOpen = Zip.Open ((const u8 *) Z.data (), Z.size ());
		unsigned nGood = 0;
		for (unsigned i = 0; bOpen && i < Zip.GetCount (); i++)
		{
			if (Zip.IsDirectory (i)) continue;
			std::vector<u8> Out (Zip.GetSize (i) + 1);
			const int nGot = Zip.Extract (i, Out.data (), Out.size ());
			std::string Want; FILE *g = fopen ((std::string (argv[2]) + "/" + Zip.GetName (i)).c_str (), "rb");
			while (g && (n = fread (Buf, 1, sizeof Buf, g)) > 0) Want.append (Buf, n);
			if (g) fclose (g);
			if (nGot == (int) Want.size () && memcmp (Out.data (), Want.data (), Want.size ()) == 0) nGood++;
		}
		chk ("deflate real (zipfile de Python): todas las entradas iguales", bOpen && nGood == 4);
		// a byte changed in the compressed data: the CRC must catch it
		std::string Z2 = Z; Z2[Z2.size () / 3] ^= 0x55;
		CZipFile Zip2 (64); unsigned nBad = 0;
		if (Zip2.Open ((const u8 *) Z2.data (), Z2.size ()))
			for (unsigned i = 0; i < Zip2.GetCount (); i++)
			{
				std::vector<u8> Out (Zip2.GetSize (i) + 1);
				if (!Zip2.IsDirectory (i) && Zip2.Extract (i, Out.data (), Out.size ()) != (int) Zip2.GetSize (i)) nBad++;
			}
		chk ("deflate real: un byte cambiado lo detecta el CRC", nBad >= 1);
	}

	char Cmd[96];
	snprintf (Cmd, sizeof Cmd, "rm -rf %s", fs.Root.c_str ());
	if (system (Cmd) != 0) printf ("no se pudo borrar %s\n", fs.Root.c_str ());
	printf ("\n%d fallos\n", fails);
	return fails ? 1 : 0;
}
