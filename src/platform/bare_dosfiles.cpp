//
// bare_dosfiles.cpp
//
// BM-DOS - is a file of the card open in DOS right now?  For the web server,
// which must not delete, rename or move what a DOS program has open: FatFs
// keeps no lock (FF_FS_LOCK 0), and removing an open file can corrupt the
// volume - a game writing its save through a handle whose clusters were freed.
//
// DOSBox keeps every open DOS file in Files[] (dos_files.cpp): the drive it is
// on and its DOS name ("GAMES\DOOM\DOOMSAV0.DSG").  For a drive that is a folder
// of the card (a localDrive, MOUNT C /dos), that folder plus the name, with '\'
// as '/', is the file on the card.  Chosen over turning FatFs's lock on: that
// lock would also stop a program opening one file twice for writing, which some
// do, and DOS programs keep behaving exactly as before this way.
//
// Compiled with DOSBox's flags (Makefile: DOSBOX_PLATFORM_SRCS), for its headers
// and its RTTI.  Read from the web server's task on core 0, which runs only
// when the emulator has yielded at a frame boundary: never in the middle of a
// DOS open or close.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "dosbox.h"
#include "dos_inc.h"
#include "dos/drives.h"

#include <cstring>
#include <string>
#include <strings.h>

namespace {

// A card path as the web server writes it ("/dos/GAMES"), whatever the mount
// wrote ("SD:/dos/", "/dos"): no drive prefix, '/' separators, no trailing '/'.
std::string CardPath (std::string Path)
{
	if (Path.size () >= 3 && strncasecmp (Path.c_str (), "SD:", 3) == 0)
	{
		Path.erase (0, 3);
	}
	for (char &c : Path)
	{
		if (c == '\\') c = '/';
	}
	while (Path.size () > 1 && Path.back () == '/')
	{
		Path.pop_back ();
	}
	if (Path.empty () || Path[0] != '/')
	{
		Path.insert (0, "/");
	}
	return Path;
}

} // namespace

/// The DOS drive letter of an open file that IS pCardPath, or lies inside the
/// folder pCardPath; 0 if DOS has nothing open there.
char BareDosFileOpen (const char *pCardPath)
{
	if (pCardPath == nullptr || pCardPath[0] == '\0')
	{
		return 0;
	}
	const std::string Target = CardPath (pCardPath);

	for (unsigned i = 0; i < DOS_FILES; i++)
	{
		DOS_File *pFile = Files[i];
		if (pFile == nullptr || !pFile->IsOpen () || pFile->GetName () == nullptr)
		{
			continue;
		}
		const unsigned nDrive = pFile->GetDrive ();
		if (nDrive >= DOS_DRIVES || Drives[nDrive] == nullptr)
		{
			continue;
		}
		localDrive *pLocal = dynamic_cast<localDrive *> (Drives[nDrive]);
		if (pLocal == nullptr)
		{
			continue;		// an image or a CD: not a file of the card
		}

		const std::string Open = CardPath (std::string (pLocal->getBasedir ()) + "/" + pFile->GetName ());
		const bool bSame = strcasecmp (Open.c_str (), Target.c_str ()) == 0;
		const bool bInside = Target == "/"
				     || (Open.size () > Target.size () && Open[Target.size ()] == '/'
					 && strncasecmp (Open.c_str (), Target.c_str (), Target.size ()) == 0);
		if (bSame || bInside)
		{
			return static_cast<char> ('A' + nDrive);
		}
	}
	return 0;
}

/// DOS's current PSP: the segment of the program running now.  It changes when
/// a program starts and when it ends (back to the shell's); the governor warms
/// up on a start.  0 before DOS exists.
unsigned BareDosCurrentPsp (void)
{
	return dos.psp ();
}
