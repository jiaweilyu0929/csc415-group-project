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
* Description:: Directory API stubs (Phase 2+). fs_getcwd / fs_setcwd keep
*   the logical cwd string in this file (path normalization only; no disk walk).
*
**************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "mfs.h"

#define FS_CWD_MAX 4096

static char g_fs_cwd[FS_CWD_MAX] = "/";

/* Used only by fs_setcwd: resolve relative paths and normalize . / .. / extra slashes. */
static int
path_canonicalize (char *out, size_t outlen, const char *cwd, const char *inpath)
	{
	char work[FS_CWD_MAX];
	char wcopy[FS_CWD_MAX];
	const char *parts[256];
	int np = 0;

	if (inpath == NULL || out == NULL || outlen < 2)
		return -1;
	if (inpath[0] == '\0')
		return -1;

	if (inpath[0] == '/')
		snprintf (work, sizeof work, "%s", inpath);
	else if (strcmp (cwd, "/") == 0)
		snprintf (work, sizeof work, "/%s", inpath);
	else
		snprintf (work, sizeof work, "%s/%s", cwd, inpath);

	strncpy (wcopy, work, sizeof wcopy - 1);
	wcopy[sizeof wcopy - 1] = '\0';

	char *saveptr = NULL;
	for (char *tok = strtok_r (wcopy, "/", &saveptr); tok != NULL;
	     tok = strtok_r (NULL, "/", &saveptr))
		{
		if (strcmp (tok, ".") == 0)
			continue;
		if (strcmp (tok, "..") == 0)
			{
			if (np > 0)
				np--;
			continue;
			}
		if (np >= (int)(sizeof parts / sizeof parts[0]))
			return -1;
		parts[np++] = tok;
		}

	if (np == 0)
		{
		out[0] = '/';
		out[1] = '\0';
		return 0;
		}

	size_t pos = 0;
	out[pos++] = '/';
	for (int i = 0; i < np; i++)
		{
		size_t L = strlen (parts[i]);
		if (pos + L + (i < np - 1 ? 1 : 0) >= outlen)
			return -1;
		memcpy (out + pos, parts[i], L);
		pos += L;
		if (i < np - 1)
			out[pos++] = '/';
		}
	out[pos] = '\0';
	return 0;
	}

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
	size_t len;

	if (pathbuffer == NULL || size == 0)
		{
		errno = EINVAL;
		return NULL;
		}
	len = strlen (g_fs_cwd);
	if (len + 1 > size)
		{
		errno = ERANGE;
		return NULL;
		}
	memcpy (pathbuffer, g_fs_cwd, len + 1);
	return pathbuffer;
	}

int
fs_setcwd (char *pathname)
	{
	char canon[FS_CWD_MAX];

	if (pathname == NULL)
		{
		errno = EINVAL;
		return -1;
		}
	if (path_canonicalize (canon, sizeof canon, g_fs_cwd, pathname) != 0)
		{
		errno = ENAMETOOLONG;
		return -1;
		}
	memcpy (g_fs_cwd, canon, strlen (canon) + 1);
	return 0;
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
