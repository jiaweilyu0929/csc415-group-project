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
* Description:: Directory API stubs (Phase 2+). fs_getcwd / fs_setcwd and
*   fs_isDir / fs_isFile (path normalize + lookup via fsInit).
*
**************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "mfs.h"
#include "fsLow.h"
#include <time.h>
#include <stdint.h>

#define FS_CWD_MAX 4096
#define FS_ROOT_DIR_BLOCKS  4
#define MAX_FILENAME        255
#define FS_FTYPE_DIR        2u
#define FS_FTYPE_REG        1u

/* Forward declaration of superblock — defined in fsInit.c */
typedef struct __attribute__((packed))
	{
	uint32_t magic;
	uint32_t version;
	uint64_t block_size;
	uint64_t total_blocks;
	uint64_t free_block_count;
	uint64_t bitmap_start_lba;
	uint64_t bitmap_block_count;
	uint64_t root_dir_start_lba;
	uint64_t root_dir_block_count;
	uint64_t first_data_lba;
	uint8_t  reserved[64];
	} fs_superblock_t;

/* Directory entry — defined in fsInit.c */
typedef struct __attribute__((packed))
	{
	char     name[256];
	uint8_t  fileType;
	uint8_t  inUse;
	uint64_t startBlock;
	uint64_t blockCount;
	uint64_t size;
	time_t   createTime;
	time_t   modifiedTime;
	time_t   accessTime;
	} fs_dirent_t;

/* Holds result of ParsePath */
typedef struct
	{
	fs_dirent_t *parent;
	int          index;
	char        *lastElementName;
	} parsepath_info;

extern fs_superblock_t g_fs_sb;

static char g_fs_cwd[FS_CWD_MAX] = "/";

extern int fs_vol_last_component_type (const char *abs_path);

extern int allocateBlocks(uint64_t count);

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

/* Turn shell paths into an absolute path (uses cwd when path is relative). */
static int
resolve_lookup_path (char *out, size_t outlen, const char *path)
	{
	if (path == NULL)
		return -1;
	return path_canonicalize (out, outlen, g_fs_cwd, path);
	}

/* Loads a directory from disk into a malloc'd buffer.
 * Takes the starting LBA block and block count from a
 * directory entry and returns a pointer to the loaded
 * directory array. Caller is responsible for freeing it.
 * Returns NULL upon failure. */
static fs_dirent_t *
LoadDir (fs_dirent_t *entry)
	{
	/* Calculate how many bytes the directory occupies on disk. */
	size_t dirBytes = (size_t)(entry->blockCount * g_fs_sb.block_size);

	fs_dirent_t *dir = malloc (dirBytes);
	if (dir == NULL)
		return NULL;

	/* Read the directory blocks from disk into our buffer. */
	if (LBAread (dir, entry->blockCount, entry->startBlock)
	    != entry->blockCount)
		{
		free (dir);
		return NULL;
		}

	return dir;
	}

/* Searches a loaded directory for an entry matching name.
 * Returns the index of the matching entry if found,
 * or -1 if no entry with that name exists.
 * This lets ParsePath and fs_mkdir know exactly which
 * slot holds the entry they are looking for. */
static int
FindEntryInDir (fs_dirent_t *dir, const char *name)
	{
	/* How many entries fit in this directory?
	 * dir[0].size holds the total byte size of the directory,
	 * so dividing by the entry size gives us the slot count. */
	int count = dir[0].size / sizeof (fs_dirent_t);

	for (int i = 0; i < count; i++)
		{
		if (dir[i].inUse && strcmp (dir[i].name, name) == 0)
			return i;
		}

	return -1;  // no name found
	}

/* ParsePath: walks a file path one component at a time and fills
 * in a parsepath_info struct so the caller knows:
 *   - which directory is the PARENT of the last element
 *   - what INDEX in that parent the last element is at (-1 if not found yet)
 *   - what the LAST ELEMENT NAME is (e.g. "bar" in "/foo/bar")
 *
 * Caller must free ppi->parent and ppi->lastElementName when done. */
static int
ParsePath (const char *path, parsepath_info *ppi)
	{
	/* Can't work with a NULL path or NULL output struct */
	if (path == NULL || ppi == NULL)
		return -1;

	/* Turn whatever the user typed (relative or absolute) into
	 * a clean absolute path like "/foo/bar" */
	char abs[FS_CWD_MAX];
	if (resolve_lookup_path (abs, sizeof abs, path) != 0)
		return -1;

	/* Build a fake directory entry for root so we can use
	 * LoadDir on it — we get root's location from the superblock */
	fs_dirent_t root_entry;
	root_entry.startBlock = g_fs_sb.root_dir_start_lba;
	root_entry.blockCount = g_fs_sb.root_dir_block_count;
	root_entry.size       = g_fs_sb.root_dir_block_count
	                        * g_fs_sb.block_size;

	/* Load the root directory from disk into memory — this is
	 * our starting point for walking the path */
	fs_dirent_t *current = LoadDir (&root_entry);
	if (current == NULL)
		return -1;

	/* Special case: path is just "/" meaning root IS the target.
	 * index -2 means "this directory itself is what was asked for" */
	if (abs[1] == '\0')
		{
		ppi->parent          = current;
		ppi->index           = -2;
		ppi->lastElementName = NULL;
		return 0;
		}

	/* Copy the path skipping the leading '/' so strtok_r can
	 * split it into individual components like "foo", "bar" */
	char buf[FS_CWD_MAX];
	strncpy (buf, abs + 1, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';

	/* token = current component we are looking at
	 * next  = the component AFTER token (NULL if token is last)
	 * We peek at next so we know when to stop walking */
	char *saveptr = NULL;
	char *token   = strtok_r (buf, "/", &saveptr);
	char *next    = strtok_r (NULL, "/", &saveptr);

	while (token != NULL)
		{
		if (next == NULL)
			{
			/* token is the LAST component — we are done walking.
			 * current is the parent directory we want.
			 * Look up token in the parent to get its index
			 * (-1 means it doesn't exist yet, which is fine for mkdir) */
			ppi->parent          = current;
			ppi->index           = FindEntryInDir (current, token);
			ppi->lastElementName = strdup (token);
			return 0;
			}

		/* token is a MIDDLE component — it must exist and must
		 * be a directory so we can keep walking into it */
		int idx = FindEntryInDir (current, token);
		if (idx == -1)
			{
			/* A middle component doesn't exist — invalid path */
			free (current);
			return -1;
			}

		/* Make sure the middle component is actually a directory
		 * and not a file — you can't walk into a file */
		if (current[idx].fileType != FS_FTYPE_DIR)
			{
			free (current);
			return -1;
			}

		/* Load the next directory from disk and free the
		 * current one since we no longer need it in memory */
		fs_dirent_t *next_dir = LoadDir (&current[idx]);
		free (current);
		if (next_dir == NULL)
			return -1;

		/* Move forward — next_dir becomes our new current,
		 * advance both token and next to the next components */
		current = next_dir;
		token   = next;
		next    = strtok_r (NULL, "/", &saveptr);
		}

	/* Should not reach here but free and return error if we do */
	free (current);
	return -1;
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
	char abs[FS_CWD_MAX];
	int t;

	if (filename == NULL)
		return 0;
	if (resolve_lookup_path (abs, sizeof abs, filename) != 0)
		return 0;
	t = fs_vol_last_component_type (abs);
	return (t == 1) ? 1 : 0;
	}

int
fs_isDir (char *pathname)
	{
	char abs[FS_CWD_MAX];
	int t;

	if (pathname == NULL)
		return 0;
	if (resolve_lookup_path (abs, sizeof abs, pathname) != 0)
		return 0;
	t = fs_vol_last_component_type (abs);
	return (t == 2) ? 1 : 0;
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
