/**************************************************************
* Class::  CSC-415-0# Fall 2025
* Name:: Jiawei Lyu, Leslie Raya, Alexandra Borders, Yeraldin Crespo
* Student IDs:: 923809065, 921813630, 913630008, 923523819
* GitHub-Name::
* Group-Name:: Team #1
* Project:: Basic File System
*
* File:: mfs.c
*
* Description:: Directory API stubs (Phase 2+) and minimal cwd
*   helpers so the shell can build. Full implementation pending.
*
**************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "mfs.h"

int
fs_mkdir (const char *pathname, mode_t mode)
	{
	(void) pathname;
	(void) mode;
	errno = ENOSYS;
	return -1;
	}

int
fs_rmdir (const char *pathname)
	{
	(void) pathname;
	errno = ENOSYS;
	return -1;
	}

fdDir *
fs_opendir (const char *pathname)
	{
	(void) pathname;
	errno = ENOSYS;
	return NULL;
	}

struct fs_diriteminfo *
fs_readdir (fdDir *dirp)
	{
	if (dirp == NULL)
		return NULL;
	errno = ENOSYS;
	return NULL;
	}

int
fs_closedir (fdDir *dirp)
	{
	if (dirp == NULL)
		return -1;
	if (dirp->di != NULL)
		free (dirp->di);
	free (dirp);
	return 0;
	}

char *
fs_getcwd (char *pathbuffer, size_t size)
	{
	if (pathbuffer == NULL || size < 2)
		return NULL;
	strncpy (pathbuffer, "/", size);
	pathbuffer[size - 1] = '\0';
	return pathbuffer;
	}

int
fs_setcwd (char *pathname)
	{
	if (pathname == NULL)
		return -1;
	if (strcmp (pathname, "/") == 0)
		return 0;
	errno = ENOSYS;
	return -1;
	}

int
fs_isFile (char *filename)
	{
	(void) filename;
	return 0;
	}

int
fs_isDir (char *pathname)
	{
	if (pathname != NULL && strcmp (pathname, "/") == 0)
		return 1;
	return 0;
	}

int
fs_delete (char *filename)
	{
	(void) filename;
	errno = ENOSYS;
	return -1;
	}

int
fs_stat (const char *path, struct fs_stat *buf)
	{
	(void) path;
	(void) buf;
	errno = ENOSYS;
	return -1;
	}
