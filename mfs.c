/**************************************************************
* Class::  CSC-415-0# Fall 2025
* Name:: Jiawei Lyu, Leslie Raya, Alexandra Borders, Yeraldin Crespo
* Student IDs:: 923809065, 921813630, 913630008, 923523819
* GitHub-Name:: jiaweilyu0929
* Group-Name:: Team #1
* Project:: Basic File System
*
* File:: mfs.c
*
* Description:: Implements the directory API for our file system.
*   Includes path resolution helpers, directory creation (fs_mkdir),
*   directory iteration (fs_opendir / fs_readdir / fs_closedir),
*   working directory tracking (fs_getcwd / fs_setcwd), and
*   type-checking helpers (fs_isFile / fs_isDir).
*
**************************************************************/
// Leslie update
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "mfs.h"
#include "fsLow.h"
#include <time.h>
#include <stdint.h>

/* Maximum length of any absolute path string we store in memory. */
#define FS_CWD_MAX 4096

/* How many 512-byte blocks every directory occupies on disk.
 * Keeping this fixed makes allocation and sizing simple. */
#define FS_ROOT_DIR_BLOCKS  4

/* Maximum number of characters allowed in a file or directory name
 * (not counting the null terminator). */
#define MAX_FILENAME        255

/* Values stored in fs_dirent_t.fileType to distinguish
 * directories from regular files. */
#define FS_FTYPE_DIR        2u
#define FS_FTYPE_REG        1u

/* ---------------------------------------------------------------
 * These two structs mirror the ones defined in fsInit.c exactly.
 * We redeclare them here so mfs.c can use them without needing
 * to expose fsInit.c internals through a header file.
 * The __attribute__((packed)) tells the compiler NOT to add any
 * hidden padding bytes between fields, so the layout in memory
 * matches the layout on disk byte-for-byte.
 * --------------------------------------------------------------- */

/* The superblock lives at block 0 and acts as a table of contents
 * for the entire volume. It tells us where everything else lives. */
typedef struct __attribute__((packed))
	{
	uint32_t magic;              /* Magic number to confirm this is our FS */
	uint32_t version;            /* File system version number             */
	uint64_t block_size;         /* Size of one block in bytes             */
	uint64_t total_blocks;       /* Total number of blocks on the volume   */
	uint64_t free_block_count;   /* How many blocks are still available    */
	uint64_t bitmap_start_lba;   /* Block number where the bitmap starts   */
	uint64_t bitmap_block_count; /* How many blocks the bitmap occupies    */
	uint64_t root_dir_start_lba; /* Block number where root directory starts */
	uint64_t root_dir_block_count; /* How many blocks the root dir occupies */
	uint64_t first_data_lba;     /* First block available for file data    */
	uint8_t  reserved[64];       /* Spare space for future fields          */
	} fs_superblock_t;

/* One directory entry describes a single file or subdirectory.
 * A directory on disk is just an array of these structs packed
 * side by side. Slots with inUse == 0 are empty and available. */
typedef struct __attribute__((packed))
	{
	char     name[256];      /* Name of the file or directory (null-terminated) */
	uint8_t  fileType;       /* FS_FTYPE_REG for files, FS_FTYPE_DIR for dirs   */
	uint8_t  inUse;          /* 1 = this slot is occupied, 0 = it is free        */
	uint64_t startBlock;     /* First disk block that holds this entry's data    */
	uint64_t blockCount;     /* How many consecutive blocks this entry uses      */
	uint64_t size;           /* Exact byte size (useful for files)               */
	time_t   createTime;     /* Timestamp: when this entry was first created     */
	time_t   modifiedTime;   /* Timestamp: when the content was last changed     */
	time_t   accessTime;     /* Timestamp: when the entry was last read          */
	} fs_dirent_t;

/* ParsePath fills one of these in so the caller has everything it
 * needs to create, find, or delete the last component of a path.
 *
 *   parent          - the directory that CONTAINS the last name,
 *                     loaded from disk into a malloc'd buffer
 *   index           - slot number of the last name inside parent
 *                     (-1 = name not found yet, fine for mkdir)
 *                     (-2 = the path was "/" so parent IS the target)
 *   lastElementName - a malloc'd copy of the final path component
 *                     e.g. "bar" for the path "/foo/bar"
 *
 * IMPORTANT: The caller must free() both parent and lastElementName
 * when it is finished with them to avoid memory leaks. */
typedef struct
	{
	fs_dirent_t *parent;
	int          index;
	char        *lastElementName;
	} parsepath_info;

/* g_fs_sb is defined and populated in fsInit.c.
 * Declaring it extern here lets mfs.c read the superblock fields
 * (like root directory location and block size) without a second copy. */
extern fs_superblock_t g_fs_sb;

/* g_fs_cwd holds the current working directory as an absolute path
 * string, e.g. "/home/student". It always starts as root "/".
 * fs_getcwd reads it; fs_setcwd updates it. */
static char g_fs_cwd[FS_CWD_MAX] = "/";

/* These two functions live in fsInit.c. We declare them extern so
 * we can call them from here without duplicating their code. */
extern int fs_vol_last_component_type (const char *abs_path);
extern int allocateBlocks (uint64_t count);


/* ---------------------------------------------------------------
 * path_canonicalize
 *
 * Turns any path (relative or absolute, possibly messy) into a
 * clean absolute path string with no ".", "..", or double slashes.
 *
 * Examples:
 *   cwd="/home",  inpath="docs/../pics"  -> "/home/pics"
 *   cwd="/",      inpath="/foo//bar"     -> "/foo/bar"
 *   cwd="/a/b/c", inpath="../.."        -> "/a"
 *
 * Parameters:
 *   out    - buffer where the result is written
 *   outlen - size of that buffer (must be at least 2)
 *   cwd    - the current working directory (used for relative paths)
 *   inpath - the path the user typed
 *
 * Returns 0 on success, -1 if the path is invalid or too long.
 * --------------------------------------------------------------- */
static int
path_canonicalize (char *out, size_t outlen, const char *cwd,
                   const char *inpath)
	{
	char work[FS_CWD_MAX];   /* Full raw path before cleaning              */
	char wcopy[FS_CWD_MAX];  /* Copy we can safely destroy with strtok_r  */
	const char *parts[256];  /* Array of valid path components after cleaning */
	int np = 0;              /* Number of valid components accumulated so far */

	if (inpath == NULL || out == NULL || outlen < 2)
		return -1;
	if (inpath[0] == '\0')
		return -1;

	/* Step 1: Build the raw combined path.
	 * If inpath starts with '/' it is already absolute, so use it as-is.
	 * Otherwise prepend the current working directory. */
	if (inpath[0] == '/')
		snprintf (work, sizeof work, "%s", inpath);
	else if (strcmp (cwd, "/") == 0)
		snprintf (work, sizeof work, "/%s", inpath);
	else
		snprintf (work, sizeof work, "%s/%s", cwd, inpath);

	/* Step 2: Make a working copy because strtok_r modifies the string
	 * by replacing '/' separators with null bytes as it goes. */
	strncpy (wcopy, work, sizeof wcopy - 1);
	wcopy[sizeof wcopy - 1] = '\0';

	/* Step 3: Walk through each component separated by '/'.
	 * "."  means "stay here" -- skip it.
	 * ".." means "go up one level" -- pop the last component.
	 * Anything else is a real name -- push it onto the parts array. */
	char *saveptr = NULL;
	for (char *tok = strtok_r (wcopy, "/", &saveptr); tok != NULL;
	     tok = strtok_r (NULL, "/", &saveptr))
		{
		if (strcmp (tok, ".") == 0)
			continue;                /* Ignore "current directory" references */
		if (strcmp (tok, "..") == 0)
			{
			if (np > 0)
				np--;                /* Go up: discard the last valid component */
			continue;
			}
		if (np >= (int)(sizeof parts / sizeof parts[0]))
			return -1;               /* Path is unreasonably deep */
		parts[np++] = tok;
		}

	/* Step 4: Reassemble the clean path from the surviving components.
	 * If nothing survived the path collapses to root "/". */
	if (np == 0)
		{
		out[0] = '/';
		out[1] = '\0';
		return 0;
		}

	size_t pos = 0;
	out[pos++] = '/';                /* Every absolute path starts with '/' */
	for (int i = 0; i < np; i++)
		{
		size_t L = strlen (parts[i]);
		if (pos + L + (i < np - 1 ? 1 : 0) >= outlen)
			return -1;               /* Result would overflow the output buffer */
		memcpy (out + pos, parts[i], L);
		pos += L;
		if (i < np - 1)
			out[pos++] = '/';        /* Separate middle components with '/' */
		}
	out[pos] = '\0';
	return 0;
	}


/* ---------------------------------------------------------------
 * resolve_lookup_path
 *
 * Thin wrapper around path_canonicalize that always uses the
 * current working directory (g_fs_cwd) as the base.
 * Called by any function that needs to turn a user-supplied path
 * into a reliable absolute path before doing disk lookups.
 * --------------------------------------------------------------- */
static int
resolve_lookup_path (char *out, size_t outlen, const char *path)
	{
	if (path == NULL)
		return -1;
	return path_canonicalize (out, outlen, g_fs_cwd, path);
	}


/* ---------------------------------------------------------------
 * LoadDir
 *
 * Reads an entire directory from disk into a freshly malloc'd buffer
 * and returns a pointer to that buffer.
 *
 * We look at the entry's startBlock (where on disk it begins) and
 * blockCount (how many consecutive blocks it occupies) to know
 * exactly what to read with LBAread.
 *
 * The returned pointer is an array of fs_dirent_t structs -- one
 * slot per possible entry in that directory.
 *
 * IMPORTANT: The caller is responsible for calling free() on the
 * returned pointer when it no longer needs the data.
 *
 * Returns NULL if memory allocation or the disk read fails.
 * --------------------------------------------------------------- */
static fs_dirent_t *
LoadDir (fs_dirent_t *entry)
	{
	/* Calculate the total byte size of the directory on disk.
	 * e.g. 4 blocks x 512 bytes/block = 2048 bytes */
	size_t dirBytes = (size_t)(entry->blockCount * g_fs_sb.block_size);

	/* Allocate a buffer big enough to hold all those bytes. */
	fs_dirent_t *dir = malloc (dirBytes);
	if (dir == NULL)
		return NULL;

	/* Read the directory's blocks from disk into our buffer.
	 * LBAread returns how many blocks it actually read, so we
	 * verify it matches what we asked for. */
	if (LBAread (dir, entry->blockCount, entry->startBlock)
	    != entry->blockCount)
		{
		free (dir);
		return NULL;
		}

	return dir;
	}


/* ---------------------------------------------------------------
 * FindEntryInDir
 *
 * Searches a loaded directory (an array of fs_dirent_t structs)
 * for an entry whose name matches the given string.
 *
 * We figure out how many slots the directory has by dividing its
 * total byte size (stored in dir[0].size) by the size of one entry.
 * We only look at slots where inUse == 1 because empty slots may
 * contain garbage from a previous deletion.
 *
 * Returns the index (slot number) of the matching entry,
 * or -1 if no entry with that name exists.
 * --------------------------------------------------------------- */
static int
FindEntryInDir (fs_dirent_t *dir, const char *name)
	{
	/* dir[0].size holds the total byte size of the directory.
	 * Dividing by the entry size tells us the maximum number of
	 * entries that can fit -- not all of them will be in use. */
	int count = dir[0].size / sizeof (fs_dirent_t);

	for (int i = 0; i < count; i++)
		{
		/* Skip empty slots -- only compare names in occupied slots. */
		if (dir[i].inUse && strcmp (dir[i].name, name) == 0)
			return i;
		}

	return -1;   /* No matching name found in this directory */
	}


/* ---------------------------------------------------------------
 * ParsePath
 *
 * The central path-walking function used by mkdir, opendir, and
 * others. It breaks a path like "/home/student/docs" into steps
 * and walks the directory tree on disk one level at a time.
 *
 * When it finishes, ppi is filled in with:
 *   ppi.parent          -- the directory that CONTAINS the last name,
 *                          loaded into memory (caller must free it)
 *   ppi.index           -- slot of the last name inside parent
 *                          (-1 = not found, ok for mkdir)
 *                          (-2 = path was "/" so parent IS the target)
 *   ppi.lastElementName -- malloc'd copy of the final name component
 *                          e.g. "docs" for "/home/student/docs"
 *                          NULL if the path was "/"
 *
 * Returns 0 on success, -1 if any middle component is missing or
 * is not a directory (i.e. the path is invalid).
 * --------------------------------------------------------------- */
static int
ParsePath (const char *path, parsepath_info *ppi)
	{
	/* We can't do anything without a path or somewhere to put results. */
	if (path == NULL || ppi == NULL)
		return -1;

	/* Turn whatever the user typed into a clean absolute path.
	 * e.g. "../../foo" from cwd "/home/student" -> "/foo" */
	char abs[FS_CWD_MAX];
	if (resolve_lookup_path (abs, sizeof abs, path) != 0)
		return -1;

	/* Build a temporary fs_dirent_t that describes the root directory
	 * so we can pass it to LoadDir and read root from disk.
	 * The real root info comes from the superblock. */
	fs_dirent_t root_entry;
	root_entry.startBlock = g_fs_sb.root_dir_start_lba;
	root_entry.blockCount = g_fs_sb.root_dir_block_count;
	root_entry.size       = g_fs_sb.root_dir_block_count
	                        * g_fs_sb.block_size;

	/* Load the root directory from disk -- this is where we start walking. */
	fs_dirent_t *current = LoadDir (&root_entry);
	if (current == NULL)
		return -1;

	/* Special case: if the canonical path is exactly "/" then root itself
	 * is the target. We signal this with index = -2. */
	if (abs[1] == '\0')
		{
		ppi->parent          = current;
		ppi->index           = -2;
		ppi->lastElementName = NULL;
		return 0;
		}

	/* Skip the leading '/' so strtok_r can split "foo/bar/baz"
	 * into the tokens "foo", "bar", "baz" one at a time. */
	char buf[FS_CWD_MAX];
	strncpy (buf, abs + 1, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';

	/* We process path components in pairs: token (current) and next.
	 * Peeking at next lets us know whether token is a middle component
	 * (must be a directory we can walk into) or the last component
	 * (where we stop and report back to the caller). */
	char *saveptr = NULL;
	char *token   = strtok_r (buf, "/", &saveptr);
	char *next    = strtok_r (NULL, "/", &saveptr);

	while (token != NULL)
		{
		if (next == NULL)
			{
			/* token is the LAST component of the path.
			 * We stop here -- current is its parent directory.
			 * We look up token to find its index (-1 if it
			 * doesn't exist yet, which is normal for mkdir). */
			ppi->parent          = current;
			ppi->index           = FindEntryInDir (current, token);
			ppi->lastElementName = strdup (token);
			return 0;
			}

		/* token is a MIDDLE component -- it must already exist
		 * and it must be a directory so we can descend into it. */
		int idx = FindEntryInDir (current, token);
		if (idx == -1)
			{
			/* A directory in the middle of the path is missing --
			 * the path is invalid and we cannot continue. */
			free (current);
			return -1;
			}

		if (current[idx].fileType != FS_FTYPE_DIR)
			{
			/* The middle component exists but is a file, not a
			 * directory. You cannot use a file as a folder. */
			free (current);
			return -1;
			}

		/* Load the subdirectory from disk, then release the memory
		 * used by the directory we just finished with. */
		fs_dirent_t *next_dir = LoadDir (&current[idx]);
		free (current);
		if (next_dir == NULL)
			return -1;

		/* Descend one level: the subdirectory we just loaded becomes
		 * the new "current", and we advance both tokens forward. */
		current = next_dir;
		token   = next;
		next    = strtok_r (NULL, "/", &saveptr);
		}

	/* We should never fall through the loop normally.
	 * If we do, something went wrong -- clean up and report failure. */
	free (current);
	return -1;
	}


/* ---------------------------------------------------------------
 * fs_mkdir
 *
 * Creates a new directory at the given path.
 *
 * Steps:
 *   1. Use ParsePath to verify the parent exists and the new name
 *      doesn't already exist inside it.
 *   2. Allocate disk blocks for the new directory.
 *   3. Write the new directory to disk with "." and ".." entries.
 *   4. Add an entry for the new directory into the parent and
 *      write the updated parent back to disk.
 *
 * Returns 0 on success, -1 on any error (errno is set).
 * --------------------------------------------------------------- */
int
fs_mkdir (const char *pathname, mode_t mode)
	{
	/* mode controls Unix permissions -- we accept it to match the
	 * standard interface but don't enforce permissions in this FS. */
	(void) mode;

	if (pathname == NULL)
		{
		errno = EINVAL;
		return -1;
		}

	/* Walk the path so we know who the parent is and whether the
	 * new name is already taken. */
	parsepath_info ppi;
	if (ParsePath (pathname, &ppi) != 0)
		{
		/* A middle component of the path doesn't exist. */
		errno = ENOENT;
		return -1;
		}

	/* ppi.index >= 0 means an entry with this name already exists
	 * in the parent -- we cannot create a duplicate. */
	if (ppi.index >= 0)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EEXIST;
		return -1;
		}

	/* ppi.index == -2 means the caller passed "/" as the path.
	 * The root directory already exists and cannot be created again. */
	if (ppi.lastElementName == NULL)
		{
		free (ppi.parent);
		errno = EINVAL;
		return -1;
		}

	/* Reject names that are too long to store in our name field. */
	if (strlen (ppi.lastElementName) > MAX_FILENAME)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENAMETOOLONG;
		return -1;
		}

	/* Ask the bitmap allocator for a contiguous run of blocks on disk
	 * for the new directory. Returns the starting block number. */
	int newStart = allocateBlocks (FS_ROOT_DIR_BLOCKS);
	if (newStart < 0)
		{
		/* Not enough free space on the volume. */
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOSPC;
		return -1;
		}

	/* Allocate a memory buffer to build the new directory's contents.
	 * calloc zeroes the buffer so all slots start out as inUse = 0. */
	size_t newDirBytes = FS_ROOT_DIR_BLOCKS * g_fs_sb.block_size;
	fs_dirent_t *newDir = calloc (FS_ROOT_DIR_BLOCKS,
	                               (size_t) g_fs_sb.block_size);
	if (newDir == NULL)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOMEM;
		return -1;
		}

	/* Get the current time once so all timestamps are consistent. */
	time_t now = time (NULL);

	/* Slot 0 is always "." -- the entry that points back to this
	 * directory itself. It lets programs refer to the directory
	 * they are currently inside. */
	strncpy (newDir[0].name, ".", MAX_FILENAME);
	newDir[0].fileType     = FS_FTYPE_DIR;
	newDir[0].inUse        = 1;
	newDir[0].startBlock   = (uint64_t) newStart;
	newDir[0].blockCount   = FS_ROOT_DIR_BLOCKS;
	newDir[0].size         = newDirBytes;
	newDir[0].createTime   = now;
	newDir[0].modifiedTime = now;
	newDir[0].accessTime   = now;

	/* Slot 1 is always ".." -- the entry that points up to the parent
	 * directory. Programs use it to navigate to the directory above.
	 * We copy the parent's startBlock and blockCount so ".." leads
	 * exactly to where the parent lives on disk. */
	strncpy (newDir[1].name, "..", MAX_FILENAME);
	newDir[1].fileType     = FS_FTYPE_DIR;
	newDir[1].inUse        = 1;
	newDir[1].startBlock   = ppi.parent[0].startBlock;
	newDir[1].blockCount   = ppi.parent[0].blockCount;
	newDir[1].size         = ppi.parent[0].size;
	newDir[1].createTime   = now;
	newDir[1].modifiedTime = now;
	newDir[1].accessTime   = now;

	/* Write the new directory's contents to the blocks we reserved. */
	if (LBAwrite (newDir, FS_ROOT_DIR_BLOCKS, (uint64_t) newStart)
	    != FS_ROOT_DIR_BLOCKS)
		{
		free (newDir);
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return -1;
		}
	free (newDir);   /* New directory is on disk -- release the buffer. */

	/* Now add the new directory as an entry inside the parent.
	 * Calculate how many slots the parent directory has total. */
	int parentSlots = (int)(ppi.parent[0].size / sizeof (fs_dirent_t));

	/* Scan the parent for a slot that is not currently in use. */
	int freeSlot = -1;
	for (int i = 0; i < parentSlots; i++)
		{
		if (!ppi.parent[i].inUse)
			{
			freeSlot = i;
			break;
			}
		}

	if (freeSlot < 0)
		{
		/* The parent directory has no empty slots left.
		 * In a more advanced implementation we would expand the parent,
		 * but for now we report that there is no space. */
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOSPC;
		return -1;
		}

	/* Clear the chosen slot to make sure no old data remains, then
	 * fill in all the fields for the new directory entry. */
	memset (&ppi.parent[freeSlot], 0, sizeof (fs_dirent_t));
	strncpy (ppi.parent[freeSlot].name, ppi.lastElementName, MAX_FILENAME);
	ppi.parent[freeSlot].name[MAX_FILENAME] = '\0';
	ppi.parent[freeSlot].fileType     = FS_FTYPE_DIR;
	ppi.parent[freeSlot].inUse        = 1;
	ppi.parent[freeSlot].startBlock   = (uint64_t) newStart;
	ppi.parent[freeSlot].blockCount   = FS_ROOT_DIR_BLOCKS;
	ppi.parent[freeSlot].size         = newDirBytes;
	ppi.parent[freeSlot].createTime   = now;
	ppi.parent[freeSlot].modifiedTime = now;
	ppi.parent[freeSlot].accessTime   = now;

	/* Write the modified parent directory back to disk so the new
	 * entry is permanently saved. Without this write the directory
	 * would only exist in memory and be lost on the next restart. */
	int rc = 0;
	if (LBAwrite (ppi.parent,
	              ppi.parent[0].blockCount,
	              ppi.parent[0].startBlock)
	    != ppi.parent[0].blockCount)
		{
		errno = EIO;
		rc = -1;
		}

	free (ppi.parent);
	free (ppi.lastElementName);
	return rc;
	}


int
fs_rmdir (const char *pathname)
	{
	parsepath_info ppi;
	fs_dirent_t *child;
	fs_dirent_t *child_entries = NULL;
	int entry_count;

	if (pathname == NULL)
		{
		errno = EINVAL;
		return -1;
		}

	if (ParsePath (pathname, &ppi) != 0)
		{
		errno = ENOENT;
		return -1;
		}

	/* ParsePath uses -2 to signal "/" (root). */
	if (ppi.index == -2 || ppi.lastElementName == NULL)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EBUSY;
		return -1;
		}

	if (ppi.index < 0)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOENT;
		return -1;
		}

	if (ppi.parent[ppi.index].fileType != FS_FTYPE_DIR)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOTDIR;
		return -1;
		}

	child = &ppi.parent[ppi.index];
	child_entries = LoadDir (child);
	if (child_entries == NULL)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return -1;
		}

	entry_count = (int) (child_entries[0].size / sizeof (fs_dirent_t));
	for (int i = 0; i < entry_count; i++)
		{
		if (!child_entries[i].inUse)
			continue;
		if (strcmp (child_entries[i].name, ".") == 0
		    || strcmp (child_entries[i].name, "..") == 0)
			continue;
		free (child_entries);
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOTEMPTY;
		return -1;
		}
	free (child_entries);

	memset (&ppi.parent[ppi.index], 0, sizeof (fs_dirent_t));
	if (LBAwrite (ppi.parent,
	              ppi.parent[0].blockCount,
	              ppi.parent[0].startBlock)
	    != ppi.parent[0].blockCount)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return -1;
		}

	free (ppi.parent);
	free (ppi.lastElementName);
	return 0;
	}


/* ---------------------------------------------------------------
 * fs_opendir
 *
 * Opens a directory so the caller can iterate through its entries
 * using fs_readdir. Think of it like fopen but for directories.
 *
 * Internally we:
 *   1. Walk the path with ParsePath to find the directory on disk.
 *   2. Load all the directory's entries from disk into memory.
 *   3. Allocate an fdDir "cursor" struct that tracks where we are
 *      in the iteration (current_index starts at 0).
 *
 * Memory layout of dirp->di:
 *   We need to store BOTH the fs_diriteminfo (returned by readdir)
 *   AND a pointer to the entries array. We do this by allocating
 *   a single block of memory that holds both back to back:
 *     [ struct fs_diriteminfo | fs_dirent_t* entries_pointer ]
 *   The entries_pointer sits immediately after the struct in memory.
 *   d_reclen is repurposed to store the total entry count since we
 *   don't need its original meaning here.
 *
 * Returns a pointer to an fdDir on success, NULL on failure (errno set).
 * The caller must call fs_closedir when done to free all memory.
 * --------------------------------------------------------------- */
fdDir *
fs_opendir (const char *pathname)
	{
	if (pathname == NULL)
		{
		errno = EINVAL;
		return NULL;
		}

	/* Walk the path to find out where the directory lives on disk. */
	parsepath_info ppi;
	if (ParsePath (pathname, &ppi) != 0)
		{
		errno = ENOENT;
		return NULL;
		}

	/* Figure out which entry actually describes our target directory.
	 *
	 * Case 1 -- index == -2: the path was "/" so ppi.parent IS root,
	 *   meaning the directory we want to open IS the parent itself.
	 *
	 * Case 2 -- index >= 0: the last component was found inside the
	 *   parent at slot ppi.index. We need to verify it's a directory
	 *   and not a regular file before we try to open it.
	 *
	 * Case 3 -- index == -1: the path doesn't exist on disk at all. */
	fs_dirent_t *target = NULL;

	if (ppi.index == -2)
		{
		/* Opening root -- ppi.parent itself is the target directory. */
		target = ppi.parent;
		}
	else if (ppi.index >= 0)
		{
		/* Verify the found entry is actually a directory.
		 * Trying to opendir on a regular file is an error. */
		if (ppi.parent[ppi.index].fileType != FS_FTYPE_DIR)
			{
			free (ppi.parent);
			free (ppi.lastElementName);
			errno = ENOTDIR;
			return NULL;
			}
		target = &ppi.parent[ppi.index];
		}
	else
		{
		/* index == -1: path doesn't exist. */
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOENT;
		return NULL;
		}

	/* Calculate the total byte size of the directory and how many
	 * entry slots it contains. We need both values going forward. */
	size_t dirBytes = (size_t)(target->blockCount * g_fs_sb.block_size);
	int    nEntries = (int)(dirBytes / sizeof (fs_dirent_t));

	/* Allocate memory and load ALL of the directory's entries from disk.
	 * We keep this array alive for the entire lifetime of the fdDir
	 * so readdir can return entries from it one at a time. */
	fs_dirent_t *entries = malloc (dirBytes);
	if (entries == NULL)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOMEM;
		return NULL;
		}

	if (LBAread (entries, target->blockCount, target->startBlock)
	    != target->blockCount)
		{
		free (entries);
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return NULL;
		}

	/* Allocate the fdDir struct that the caller will hold onto.
	 * This is our "directory handle" -- like a file descriptor for dirs. */
	fdDir *dirp = malloc (sizeof (fdDir));
	if (dirp == NULL)
		{
		free (entries);
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOMEM;
		return NULL;
		}

	dirp->d_reclen        = (unsigned short) sizeof (fdDir);
	dirp->dir_start_lba   = target->startBlock;
	dirp->dir_block_count = (uint32_t) target->blockCount;
	dirp->current_index   = 0;   /* Start iteration from the first slot */

	/* Allocate a combined block for the readdir result struct AND the
	 * pointer to our entries array. Layout in memory:
	 *   [ struct fs_diriteminfo (returned to caller by readdir)   ]
	 *   [ fs_dirent_t* (hidden pointer to the full entries array) ]
	 *
	 * Storing both in one allocation avoids a second malloc and
	 * makes cleanup in fs_closedir straightforward. */
	dirp->di = malloc (sizeof (struct fs_diriteminfo) + sizeof (fs_dirent_t *));
	if (dirp->di == NULL)
		{
		free (entries);
		free (dirp);
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOMEM;
		return NULL;
		}

	/* Repurpose d_reclen to store the total entry count.
	 * readdir needs to know when it has walked past the last slot. */
	dirp->di->d_reclen = (unsigned short) nEntries;

	/* Store the entries pointer in the extra space immediately after
	 * the fs_diriteminfo struct. "(dirp->di + 1)" advances past the
	 * struct by exactly sizeof(struct fs_diriteminfo) bytes, landing
	 * us at the spot reserved for our hidden fs_dirent_t pointer. */
	fs_dirent_t **ep = (fs_dirent_t **)(dirp->di + 1);
	*ep = entries;

	free (ppi.parent);
	free (ppi.lastElementName);
	return dirp;
	}


/* ---------------------------------------------------------------
 * fs_readdir
 *
 * Returns information about the next entry in a directory that was
 * opened with fs_opendir. Call it repeatedly in a loop until it
 * returns NULL, which signals that every entry has been returned.
 *
 * Skips slots where inUse == 0 (empty or deleted entries) so the
 * caller only ever sees real, live directory entries.
 *
 * The returned pointer always points to the same struct (dirp->di)
 * and is overwritten on every call. If you need to keep the data
 * from one call while making the next, copy the fields you need.
 *
 * Returns NULL when there are no more entries to return.
 * --------------------------------------------------------------- */
struct fs_diriteminfo *
fs_readdir (fdDir *dirp)
	{
	if (dirp == NULL || dirp->di == NULL)
		return NULL;

	/* Recover the total entry count and the entries array pointer
	 * that fs_opendir packed into the di allocation. */
	int          nEntries = (int) dirp->di->d_reclen;
	fs_dirent_t **ep      = (fs_dirent_t **)(dirp->di + 1);
	fs_dirent_t  *entries = *ep;

	/* Advance past any empty slots (inUse == 0) until we land on
	 * a real entry or run off the end of the directory. */
	while (dirp->current_index < (uint32_t) nEntries
	       && !entries[dirp->current_index].inUse)
		dirp->current_index++;

	/* If we walked past the last slot, iteration is complete. */
	if (dirp->current_index >= (uint32_t) nEntries)
		return NULL;

	/* Grab a pointer to the current entry, then advance the index
	 * so the next call to readdir picks up at the following slot. */
	fs_dirent_t *de = &entries[dirp->current_index];
	dirp->current_index++;

	/* Copy the entry's data into the result struct.
	 * We write d_reclen back after setting fileType because both
	 * fields share the same struct and we must not lose the entry
	 * count that readdir depends on for future calls. */
	dirp->di->fileType = de->fileType;
	dirp->di->d_reclen = (unsigned short) nEntries;
	strncpy (dirp->di->d_name, de->name, 255);
	dirp->di->d_name[255] = '\0';   /* Guarantee null terminator */

	return dirp->di;
	}


/* ---------------------------------------------------------------
 * fs_closedir
 *
 * Releases all memory that was allocated by fs_opendir.
 * Must always be called when you are done with a directory to
 * avoid memory leaks. Think of it like fclose for directories.
 *
 * Frees in order:
 *   1. The entries array that was loaded from disk.
 *   2. The di allocation (which also contains the entries pointer).
 *   3. The fdDir struct itself.
 *
 * Returns 0 on success, -1 if dirp is NULL.
 * --------------------------------------------------------------- */
int
fs_closedir (fdDir *dirp)
	{
	if (dirp == NULL)
		return -1;

	if (dirp->di != NULL)
		{
		/* The entries array pointer lives just after the di struct.
		 * We retrieve it the same way fs_opendir stored it, then
		 * free the entries array before freeing di itself. */
		fs_dirent_t **ep = (fs_dirent_t **)(dirp->di + 1);
		if (*ep != NULL)
			free (*ep);
		free (dirp->di);
		}

	free (dirp);
	return 0;
	}


/* ---------------------------------------------------------------
 * fs_getcwd
 *
 * Copies the current working directory path into pathbuffer.
 * Works like the standard POSIX getcwd -- the caller provides the
 * buffer and its size; we fill it in.
 *
 * Returns pathbuffer on success, NULL if the buffer is too small
 * (errno = ERANGE) or the arguments are invalid (errno = EINVAL).
 * --------------------------------------------------------------- */
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

	/* Make sure the path plus its null terminator fits in the buffer. */
	if (len + 1 > size)
		{
		errno = ERANGE;
		return NULL;
		}

	memcpy (pathbuffer, g_fs_cwd, len + 1);
	return pathbuffer;
	}


/* ---------------------------------------------------------------
 * fs_setcwd
 *
 * Changes the current working directory to pathname.
 * Works like the standard POSIX chdir. The path is cleaned and
 * normalized before being stored so relative paths like "../foo"
 * resolve correctly from wherever we currently are.
 *
 * Note: this updates the in-memory path only. A fuller
 * implementation would also verify the path exists on disk first.
 *
 * Returns 0 on success, -1 on error (errno set).
 * --------------------------------------------------------------- */
int
fs_setcwd (char *pathname)
	{
	char canon[FS_CWD_MAX];

	if (pathname == NULL)
		{
		errno = EINVAL;
		return -1;
		}

	/* Resolve and clean the path relative to where we are now. */
	if (path_canonicalize (canon, sizeof canon, g_fs_cwd, pathname) != 0)
		{
		errno = ENAMETOOLONG;
		return -1;
		}

	/* Replace the stored working directory with the cleaned path. */
	memcpy (g_fs_cwd, canon, strlen (canon) + 1);
	return 0;
	}


/* ---------------------------------------------------------------
 * fs_isFile
 *
 * Returns 1 if the given path refers to a regular file,
 * or 0 if it doesn't exist, is a directory, or path is NULL.
 * --------------------------------------------------------------- */
int
fs_isFile (char *filename)
	{
	char abs[FS_CWD_MAX];
	int t;

	if (filename == NULL)
		return 0;

	/* Turn the path into a clean absolute path first. */
	if (resolve_lookup_path (abs, sizeof abs, filename) != 0)
		return 0;

	/* Ask fsInit.c to look up the last component and return its type.
	 * FS_FTYPE_REG (1) means it is a regular file. */
	t = fs_vol_last_component_type (abs);
	return (t == 1) ? 1 : 0;
	}


/* ---------------------------------------------------------------
 * fs_isDir
 *
 * Returns 1 if the given path refers to a directory,
 * or 0 if it doesn't exist, is a regular file, or path is NULL.
 * --------------------------------------------------------------- */
int
fs_isDir (char *pathname)
	{
	char abs[FS_CWD_MAX];
	int t;

	if (pathname == NULL)
		return 0;

	/* Turn the path into a clean absolute path first. */
	if (resolve_lookup_path (abs, sizeof abs, pathname) != 0)
		return 0;

	/* FS_FTYPE_DIR (2) means it is a directory. */
	t = fs_vol_last_component_type (abs);
	return (t == 2) ? 1 : 0;
	}


int
fs_delete (char *filename)
	{
	parsepath_info ppi;

	if (filename == NULL)
		{
		errno = EINVAL;
		return -1;
		}

	if (ParsePath (filename, &ppi) != 0)
		{
		errno = ENOENT;
		return -1;
		}

	if (ppi.index < 0)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOENT;
		return -1;
		}

	if (ppi.parent[ppi.index].fileType != FS_FTYPE_REG)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EISDIR;
		return -1;
		}

	memset (&ppi.parent[ppi.index], 0, sizeof (fs_dirent_t));
	if (LBAwrite (ppi.parent,
	              ppi.parent[0].blockCount,
	              ppi.parent[0].startBlock)
	    != ppi.parent[0].blockCount)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return -1;
		}

	free (ppi.parent);
	free (ppi.lastElementName);
	return 0;
	}


/* ---------------------------------------------------------------
 * fs_stat
 *
 * Fills in a fs_stat struct with metadata about the file or
 * directory at the given path. This is similar to the standard
 * POSIX stat() call -- it lets programs inspect a file's size,
 * timestamps, and disk usage without actually reading its data.
 *
 * We use ParsePath to walk to the entry on disk, then copy the
 * relevant fields from the fs_dirent_t into buf.
 *
 * Special case: if the path is "/" we read the "." entry from
 * root (index 0) since root has no parent entry to look up.
 *
 * Fields filled in:
 *   st_size        -- exact byte size stored in the directory entry
 *   st_blksize     -- our volume's block size (from the superblock)
 *   st_blocks      -- number of 512-byte units the entry occupies
 *                     (POSIX convention is always 512-byte units
 *                     regardless of actual block size)
 *   st_accesstime  -- time the entry was last accessed
 *   st_modtime     -- time the entry's content was last modified
 *   st_createtime  -- time the entry was originally created
 *
 * Returns 0 on success, -1 on error (errno set).
 * --------------------------------------------------------------- */
int
fs_stat (const char *path, struct fs_stat *buf)
	{
	if (path == NULL || buf == NULL)
		{
		errno = EINVAL;
		return -1;
		}

	parsepath_info ppi;
	if (ParsePath (path, &ppi) != 0)
		{
		errno = ENOENT;
		return -1;
		}

	fs_dirent_t *de;

	if (ppi.index == -2)
		de = &ppi.parent[0];
	else if (ppi.index >= 0)
		de = &ppi.parent[ppi.index];
	else
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOENT;
		return -1;
		}

	buf->st_size       = (off_t)     de->size;
	buf->st_blksize    = (blksize_t) g_fs_sb.block_size;
	buf->st_blocks     = (blkcnt_t) (de->blockCount * g_fs_sb.block_size / 512);
	buf->st_accesstime = de->accessTime;
	buf->st_modtime    = de->modifiedTime;
	buf->st_createtime = de->createTime;

	free (ppi.parent);
	free (ppi.lastElementName);
	return 0;
	}

#define FS_FILE_DATA_BLOCKS 1u

int
mfs_volume_open (char *filename, int flags, mfs_b_open_ctx *ctx)
	{
	parsepath_info ppi;

	if (filename == NULL || ctx == NULL)
		{
		errno = EINVAL;
		return -1;
		}
	memset (ctx, 0, sizeof (*ctx));

	if (ParsePath (filename, &ppi) != 0)
		{
		errno = ENOENT;
		return -1;
		}

	if (ppi.index == -2)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EISDIR;
		return -1;
		}

	if (ppi.index >= 0)
		{
		if (ppi.parent[ppi.index].fileType == FS_FTYPE_DIR)
			{
			free (ppi.parent);
			free (ppi.lastElementName);
			errno = EISDIR;
			return -1;
			}
		if (ppi.parent[ppi.index].fileType != FS_FTYPE_REG)
			{
			free (ppi.parent);
			free (ppi.lastElementName);
			errno = EINVAL;
			return -1;
			}
		ctx->parent_dir = ppi.parent;
		ctx->parent_start_lba = ppi.parent[0].startBlock;
		ctx->parent_block_count = (uint32_t) ppi.parent[0].blockCount;
		ctx->entry_index = ppi.index;
		ctx->file_start_lba = ppi.parent[ppi.index].startBlock;
		ctx->file_block_count = (uint32_t) ppi.parent[ppi.index].blockCount;
		ctx->file_size = ppi.parent[ppi.index].size;
		ctx->fs_block_size = g_fs_sb.block_size;
		free (ppi.lastElementName);
		return 0;
		}

	if (!(flags & O_CREAT))
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOENT;
		return -1;
		}
	if (ppi.lastElementName == NULL)
		{
		free (ppi.parent);
		errno = EINVAL;
		return -1;
		}
	if (strlen (ppi.lastElementName) > MAX_FILENAME)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENAMETOOLONG;
		return -1;
		}

	int newStart = allocateBlocks (FS_FILE_DATA_BLOCKS);
	if (newStart < 0)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOSPC;
		return -1;
		}

	int parentSlots = (int) (ppi.parent[0].size / sizeof (fs_dirent_t));
	int freeSlot = -1;
	for (int i = 0; i < parentSlots; i++)
		{
		if (!ppi.parent[i].inUse)
			{
			freeSlot = i;
			break;
			}
		}
	if (freeSlot < 0)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = ENOSPC;
		return -1;
		}

	time_t now = time (NULL);
	memset (&ppi.parent[freeSlot], 0, sizeof (fs_dirent_t));
	strncpy (ppi.parent[freeSlot].name, ppi.lastElementName, MAX_FILENAME);
	ppi.parent[freeSlot].name[MAX_FILENAME] = '\0';
	ppi.parent[freeSlot].fileType     = FS_FTYPE_REG;
	ppi.parent[freeSlot].inUse        = 1;
	ppi.parent[freeSlot].startBlock   = (uint64_t) newStart;
	ppi.parent[freeSlot].blockCount   = FS_FILE_DATA_BLOCKS;
	ppi.parent[freeSlot].size         = 0;
	ppi.parent[freeSlot].createTime   = now;
	ppi.parent[freeSlot].modifiedTime = now;
	ppi.parent[freeSlot].accessTime   = now;

	if (LBAwrite (ppi.parent,
	              ppi.parent[0].blockCount,
	              ppi.parent[0].startBlock)
	    != ppi.parent[0].blockCount)
		{
		free (ppi.parent);
		free (ppi.lastElementName);
		errno = EIO;
		return -1;
		}

	ctx->parent_dir = ppi.parent;
	ctx->parent_start_lba = ppi.parent[0].startBlock;
	ctx->parent_block_count = (uint32_t) ppi.parent[0].blockCount;
	ctx->entry_index = freeSlot;
	ctx->file_start_lba = (uint64_t) newStart;
	ctx->file_block_count = FS_FILE_DATA_BLOCKS;
	ctx->file_size = 0;
	ctx->fs_block_size = g_fs_sb.block_size;
	free (ppi.lastElementName);
	return 0;
	}
