//
// bare_cardfree.cpp
//
// BM-DOS - free space on the SD card, for the web server's page.  Circle's
// newlib has no statvfs (it always fails), so FatFs is asked directly - in a
// file of its own, because fatfs/ff.h and newlib's dirent.h both define DIR.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"

#include <fatfs/ff.h>

u64 BareCardFreeBytes (void)
{
	DWORD nClusters = 0;
	FATFS *pFS = nullptr;
	if (f_getfree ("SD:", &nClusters, &pFS) != FR_OK || pFS == nullptr)
	{
		return 0;
	}
#if FF_MAX_SS != FF_MIN_SS
	const u64 nSector = pFS->ssize;
#else
	const u64 nSector = FF_MAX_SS;
#endif
	return static_cast<u64> (nClusters) * pFS->csize * nSector;
}
