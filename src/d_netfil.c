// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2014 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d_netfil.c
/// \brief Transfer a file using HSendPacket.

#include <stdio.h>
#ifndef _WIN32_WCE
#ifdef __OS2__
#include <sys/types.h>
#endif // __OS2__
#include <sys/stat.h>
#endif

#if !defined (UNDER_CE)
#include <time.h>
#endif

#if ((defined (_WIN32) && !defined (_WIN32_WCE)) || defined (__DJGPP__)) && !defined (_XBOX)
#include <io.h>
#include <direct.h>
#elif !defined (_WIN32_WCE) && !(defined (_XBOX) && !defined (__GNUC__))
#include <sys/types.h>
#include <dirent.h>
#include <utime.h>
#endif

#ifdef __GNUC__
#include <unistd.h>
#include <limits.h>
#elif defined (_WIN32) && !defined (_WIN32_WCE)
#include <sys/utime.h>
#endif
#ifdef __DJGPP__
#include <dir.h>
#include <utime.h>
#endif

#include "doomdef.h"
#include "doomstat.h"
#include "d_enet.h"
#include "d_main.h"
#include "g_game.h"
#include "i_system.h"
#include "m_argv.h"
#include "w_wad.h"
#include "d_netfil.h"
#include "z_zone.h"
#include "byteptr.h"
#include "p_setup.h"
#include "m_misc.h"
#include "m_menu.h"
#include "md5.h"
#include "filesrch.h"

#include <errno.h>

static void SendFile(INT32 node, const char *filename, UINT8 fileid);

// sender structure
typedef struct filetx_s
{
	INT32 ram;
	char *filename; // name of the file or ptr of the data in ram
	UINT32 size;
	UINT8 fileid;
	INT32 node; // destination
	struct filetx_s *next; // a queue
} filetx_t;

// current transfers (one for each node)
typedef struct filetran_s
{
	filetx_t *txlist;
	UINT32 position;
	FILE *currentfile;
} filetran_t;
static filetran_t transfer[MAXNETNODES];

// read time of file: stat _stmtime
// write time of file: utime

// receiver structure
INT32 fileneedednum;
fileneeded_t fileneeded[MAX_WADFILES];
char downloaddir[256] = "DOWNLOAD";

#ifdef CLIENT_LOADINGSCREEN
// for cl loading screen
INT32 lastfilenum = 0;
#endif

/** Fills a serverinfo packet with information about wad files loaded.
  *
  * \todo Give this function a better name since it is in global scope.
  */
UINT8 *PutFileNeeded(void)
{
	// NET TODO
	return NULL;
}

// parse the serverinfo packet and fill fileneeded table on client
void D_ParseFileneeded(INT32 fileneedednum_parm, UINT8 *fileneededstr)
{
	INT32 i;
	UINT8 *p;
	UINT8 filestatus;

	fileneedednum = fileneedednum_parm;
	p = (UINT8 *)fileneededstr;
	for (i = 0; i < fileneedednum; i++)
	{
		fileneeded[i].status = FS_NOTFOUND;
		filestatus = READUINT8(p);
		fileneeded[i].important = (UINT8)(filestatus & 3);
		fileneeded[i].willsend = (UINT8)(filestatus >> 4);
		fileneeded[i].totalsize = READUINT32(p);
		fileneeded[i].phandle = NULL;
		READSTRINGN(p, fileneeded[i].filename, MAX_WADPATH);
		READMEM(p, fileneeded[i].md5sum, 16);
	}
}

void CL_PrepareDownloadSaveGame(const char *tmpsave)
{
	fileneedednum = 1;
	fileneeded[0].status = FS_REQUESTED;
	fileneeded[0].totalsize = UINT32_MAX;
	fileneeded[0].phandle = NULL;
	memset(fileneeded[0].md5sum, 0, 16);
	strcpy(fileneeded[0].filename, tmpsave);
}

/** Checks the server to see if we CAN download all the files,
  * before starting to create them and requesting.
  */
boolean CL_CheckDownloadable(void)
{
	UINT8 i,dlstatus = 0;

	for (i = 0; i < fileneedednum; i++)
		if (fileneeded[i].status != FS_FOUND && fileneeded[i].status != FS_OPEN && fileneeded[i].important)
		{
			if (fileneeded[i].willsend == 1)
				continue;

			if (fileneeded[i].willsend == 0)
				dlstatus = 1;
			else //if (fileneeded[i].willsend == 2)
				dlstatus = 2;
		}

	// Downloading locally disabled
	if (!dlstatus && M_CheckParm("-nodownload"))
		dlstatus = 3;

	if (!dlstatus)
		return true;

	// not downloadable, put reason in console
	CONS_Alert(CONS_NOTICE, M_GetText("You need additional files to connect to this server:\n"));
	for (i = 0; i < fileneedednum; i++)
		if (fileneeded[i].status != FS_FOUND && fileneeded[i].status != FS_OPEN && fileneeded[i].important)
		{
			CONS_Printf(" * \"%s\" (%dK)", fileneeded[i].filename, fileneeded[i].totalsize >> 10);

				if (fileneeded[i].status == FS_NOTFOUND)
					CONS_Printf(M_GetText(" not found, md5: "));
				else if (fileneeded[i].status == FS_MD5SUMBAD)
					CONS_Printf(M_GetText(" wrong version, md5: "));

			{
				INT32 j;
				char md5tmp[33];
				for (j = 0; j < 16; j++)
					sprintf(&md5tmp[j*2], "%02x", fileneeded[i].md5sum[j]);
				CONS_Printf("%s", md5tmp);
			}
			CONS_Printf("\n");
		}

	switch (dlstatus)
	{
		case 1:
			CONS_Printf(M_GetText("Some files are larger than the server is willing to send.\n"));
			break;
		case 2:
			CONS_Printf(M_GetText("The server is not allowing download requests.\n"));
			break;
		case 3:
			CONS_Printf(M_GetText("All files downloadable, but you have chosen to disable downloading locally.\n"));
			break;
	}
	return false;
}

/** Send requests for files in the ::fileneeded table with a status of
  * ::FS_NOTFOUND.
  */
boolean CL_SendRequestFile(void)
{
	// NET TODO
	return true;
}

// client check if the fileneeded aren't already loaded or on the disk
INT32 CL_CheckFiles(void)
{
	INT32 i, j;
	char wadfilename[MAX_WADPATH];
	INT32 ret = 1;

//	if (M_CheckParm("-nofiles"))
//		return 1;

	// the first is the iwad (the main wad file)
	// we don't care if it's called srb2.srb or srb2.wad.
	// Never download the IWAD, just assume it's there and identical
	fileneeded[0].status = FS_OPEN;

	// Modified game handling -- check for an identical file list
	// must be identical in files loaded AND in order
	// Return 2 on failure -- disconnect from server
	if (modifiedgame)
	{
		CONS_Debug(DBG_NETPLAY, "game is modified; only doing basic checks\n");
		for (i = 1, j = 1; i < fileneedednum || j < numwadfiles;)
		{
			if (i < fileneedednum && !fileneeded[i].important)
			{
				// Eh whatever, don't care
				++i;
				continue;
			}
			if (j < numwadfiles && W_VerifyNMUSlumps(wadfiles[j]->filename))
			{
				// unimportant on our side. still don't care.
				++j;
				continue;
			}

			// If this test is true, we've reached the end of one file list
			// and the other still has a file that's important
			if (i >= fileneedednum || j >= numwadfiles)
				return 2;

			// for the sake of speed, only bother with a md5 check
			if (memcmp(wadfiles[j]->md5sum, fileneeded[i].md5sum, 16))
				return 2;

			// it's accounted for! let's keep going.
			CONS_Debug(DBG_NETPLAY, "'%s' accounted for\n", fileneeded[i].filename);
			fileneeded[i].status = FS_OPEN;
			++i;
			++j;
		}
		return 1;
	}

	for (i = 1; i < fileneedednum; i++)
	{
		CONS_Debug(DBG_NETPLAY, "searching for '%s' ", fileneeded[i].filename);

		// check in allready loaded files
		for (j = 1; wadfiles[j]; j++)
		{
			nameonly(strcpy(wadfilename, wadfiles[j]->filename));
			if (!stricmp(wadfilename, fileneeded[i].filename) &&
				!memcmp(wadfiles[j]->md5sum, fileneeded[i].md5sum, 16))
			{
				CONS_Debug(DBG_NETPLAY, "already loaded\n");
				fileneeded[i].status = FS_OPEN;
				break;
			}
		}
		if (fileneeded[i].status != FS_NOTFOUND || !fileneeded[i].important)
			continue;

		fileneeded[i].status = findfile(fileneeded[i].filename, fileneeded[i].md5sum, true);
		CONS_Debug(DBG_NETPLAY, "found %d\n", fileneeded[i].status);
		if (fileneeded[i].status != FS_FOUND)
			ret = 0;
	}
	return ret;
}

// load it now
void CL_LoadServerFiles(void)
{
	INT32 i;

//	if (M_CheckParm("-nofiles"))
//		return;

	for (i = 1; i < fileneedednum; i++)
	{
		if (fileneeded[i].status == FS_OPEN)
			continue; // already loaded
		else if (fileneeded[i].status == FS_FOUND)
		{
			P_AddWadFile(fileneeded[i].filename, NULL);
			G_SetGameModified(true);
			fileneeded[i].status = FS_OPEN;
		}
		else if (fileneeded[i].status == FS_MD5SUMBAD)
		{
			// If the file is marked important, don't even bother proceeding.
			if (fileneeded[i].important)
				I_Error("Wrong version of important file %s", fileneeded[i].filename);

			// If it isn't, no need to worry the user with a console message,
			// although it can't hurt to put something in the debug file.

			// ...but wait a second. What if the local version is "important"?
			if (!W_VerifyNMUSlumps(fileneeded[i].filename))
				I_Error("File %s should only contain music and sound effects!",
					fileneeded[i].filename);

			// Okay, NOW we know it's safe. Whew.
			P_AddWadFile(fileneeded[i].filename, NULL);
			if (fileneeded[i].important)
				G_SetGameModified(true);
			fileneeded[i].status = FS_OPEN;
			DEBFILE(va("File %s found but with different md5sum\n", fileneeded[i].filename));
		}
		else if (fileneeded[i].important)
			I_Error("Try to load file %s with status of %d\n", fileneeded[i].filename,
				fileneeded[i].status);
	}
}

// little optimization to test if there is a file in the queue
static INT32 filetosend = 0;

static void SendFile(INT32 node, const char *filename, UINT8 fileid)
{
	filetx_t **q;
	filetx_t *p;
	INT32 i;
	char wadfilename[MAX_WADPATH];

	q = &transfer[node].txlist;
	while (*q)
		q = &((*q)->next);
	p = *q = (filetx_t *)malloc(sizeof (filetx_t));
	if (p)
		memset(p, 0, sizeof (filetx_t));
	else
		I_Error("SendFile: No more ram\n");
	p->filename = (char *)malloc(MAX_WADPATH);
	if (!p->filename)
		I_Error("SendFile: No more ram\n");

	// a minimum of security, can get only file in srb2 direcory
	strlcpy(p->filename, filename, MAX_WADPATH);
	nameonly(p->filename);

	// check first in wads loaded the majority of case
	for (i = 0; wadfiles[i]; i++)
	{
		strlcpy(wadfilename, wadfiles[i]->filename, MAX_WADPATH);
		nameonly(wadfilename);
		if (!stricmp(wadfilename, p->filename))
		{
			// copy filename with full path
			strlcpy(p->filename, wadfiles[i]->filename, MAX_WADPATH);
			break;
		}
	}

	if (!wadfiles[i])
	{
		DEBFILE(va("%s not found in wadfiles\n", filename));
		// this formerly checked if (!findfile(p->filename, NULL, true))

		// not found
		// don't inform client (probably hacker)
		DEBFILE(va("Client %d request %s: not found\n", node, filename));
		free(p->filename);
		free(p);
		*q = NULL;
		return;
	}

	if (wadfiles[i]->filesize > (UINT32)cv_maxsend.value * 1024)
	{
		// too big
		// don't inform client (client sucks, man)
		DEBFILE(va("Client %d request %s: file too big, not sending\n", node, filename));
		free(p->filename);
		free(p);
		*q = NULL;
		return;
	}

	DEBFILE(va("Sending file %s (id=%d) to %d\n", filename, fileid, node));
	p->ram = SF_FILE;
	p->fileid = fileid;
	p->next = NULL; // end of list
	filetosend++;
}

void SendRam(INT32 node, void *data, size_t size, freemethod_t freemethod, UINT8 fileid)
{
	filetx_t **q;
	filetx_t *p;

	q = &transfer[node].txlist;
	while (*q)
		q = &((*q)->next);
	p = *q = (filetx_t *)malloc(sizeof (filetx_t));
	if (p)
		memset(p, 0, sizeof (filetx_t));
	else
		I_Error("SendRam: No more ram\n");
	p->ram = freemethod;
	p->filename = data;
	p->size = (UINT32)size;
	p->fileid = fileid;
	p->next = NULL; // end of list

	DEBFILE(va("Sending ram %p(size:%u) to %d (id=%u)\n",p->filename,p->size,node,fileid));

	filetosend++;
}

static void EndSend(INT32 node)
{
	filetx_t *p = transfer[node].txlist;
	switch (p->ram)
	{
		case SF_FILE:
			if (transfer[node].currentfile)
				fclose(transfer[node].currentfile);
			free(p->filename);
			break;
		case SF_Z_RAM:
			Z_Free(p->filename);
			break;
		case SF_RAM:
			free(p->filename);
		case SF_NOFREERAM:
			break;
	}
	transfer[node].txlist = p->next;
	transfer[node].currentfile = NULL;
	free(p);
	filetosend--;
}

static void AbortSendFiles(INT32 node)
{
	while (transfer[node].txlist)
		EndSend(node);
}

void CloseNetFile(void)
{
	INT32 i;
	// is sending?
	for (i = 0; i < MAXNETNODES; i++)
		AbortSendFiles(i);

	// receiving a file?
	for (i = 0; i < MAX_WADFILES; i++)
		if (fileneeded[i].status == FS_DOWNLOADING && fileneeded[i].phandle)
		{
			fclose(fileneeded[i].phandle);
			// file is not complete delete it
			remove(fileneeded[i].filename);
		}

	// remove FILEFRAGMENT from acknledge list
	//Net_AbortPacketType(PT_FILEFRAGMENT);
}

// functions cut and pasted from doomatic :)

void nameonly(char *s)
{
	size_t j, len;
	void *ns;

	for (j = strlen(s); j != (size_t)-1; j--)
		if ((s[j] == '\\') || (s[j] == ':') || (s[j] == '/'))
		{
			ns = &(s[j+1]);
			len = strlen(ns);
			if (false)
				M_Memcpy(s, ns, len+1);
			else
				memmove(s, ns, len+1);
			return;
		}
}

// Returns the length in characters of the last element of a path.
size_t nameonlylength(const char *s)
{
	size_t j, len = strlen(s);

	for (j = len; j != (size_t)-1; j--)
		if ((s[j] == '\\') || (s[j] == ':') || (s[j] == '/'))
			return len - j - 1;

	return len;
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

filestatus_t checkfilemd5(char *filename, const UINT8 *wantedmd5sum)
{
#if defined (NOMD5) || defined (_arch_dreamcast)
	(void)wantedmd5sum;
	(void)filename;
#else
	FILE *fhandle;
	UINT8 md5sum[16];

	if (!wantedmd5sum)
		return FS_FOUND;

	fhandle = fopen(filename, "rb");
	if (fhandle)
	{
		md5_stream(fhandle,md5sum);
		fclose(fhandle);
		if (!memcmp(wantedmd5sum, md5sum, 16))
			return FS_FOUND;
		return FS_MD5SUMBAD;
	}

	I_Error("Couldn't open %s for md5 check", filename);
#endif
	return FS_FOUND; // will never happen, but makes the compiler shut up
}

filestatus_t findfile(char *filename, const UINT8 *wantedmd5sum, boolean completepath)
{
	filestatus_t homecheck = filesearch(filename, srb2home, wantedmd5sum, false, 10);
	if (homecheck == FS_FOUND)
		return filesearch(filename, srb2home, wantedmd5sum, completepath, 10);

	homecheck = filesearch(filename, srb2path, wantedmd5sum, false, 10);
	if (homecheck == FS_FOUND)
		return filesearch(filename, srb2path, wantedmd5sum, completepath, 10);

#ifdef _arch_dreamcast
	return filesearch(filename, "/cd", wantedmd5sum, completepath, 10);
#else
	return filesearch(filename, ".", wantedmd5sum, completepath, 10);
#endif
}
