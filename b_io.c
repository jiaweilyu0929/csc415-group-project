/**************************************************************
* Class::  CSC-415-3# Fall 2025
* Name:: Leslie Raya
* Student IDs::921813630
* GitHub-Name::jiaweilyu0929
* Group-Name:: Team #1
* Project:: Basic File System
*
* File:: b_io.c
*
* Description:: Basic File System - Key File I/O Operations
*
**************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "b_io.h"
#include "mfs.h"
#include "fsLow.h"

#define MAXFCBS 20
#define BLOCK_SIZE 512

typedef struct b_fcb
	{
	char * buf;
	int index;
	int buflen;

	int fd;
	int filePos;
	int fileSize;
	int flags;

	/* Volume-backed file (mfs_volume_open); fd == -1, use vol_* */
	int vol_open;
	mfs_b_open_ctx vol;
	uint64_t vol_file_size;
	} b_fcb;

b_fcb fcbArray[MAXFCBS];

int startup = 0;

static uint64_t
vol_capacity_bytes (const b_fcb *f)
	{
	return f->vol.file_block_count * f->vol.fs_block_size;
	}

static void
vol_reset_buf_cursor (b_fcb *f)
	{
	f->buflen = 0;
	f->index = 0;
	}

static int
b_read_volume (b_io_fd fd, char *buffer, int count)
	{
	b_fcb *f = &fcbArray[fd];
	uint64_t bs = f->vol.fs_block_size;
	int copied = 0;

	if (bs == 0 || f->vol.file_block_count == 0 || count <= 0)
		return 0;

	while (copied < count && (uint64_t) f->filePos < f->vol_file_size)
		{
		uint64_t filePos = (uint64_t) f->filePos;
		uint64_t blk = filePos / bs;
		uint64_t off = filePos % bs;
		uint64_t remainFile = f->vol_file_size - filePos;
		uint64_t remainBlk = bs - off;
		int need = count - copied;
		int chunk = need;

		if (blk >= f->vol.file_block_count)
			break;
		if ((uint64_t) chunk > remainFile)
			chunk = (int) remainFile;
		if ((uint64_t) chunk > remainBlk)
			chunk = (int) remainBlk;

		if (LBAread (f->buf, 1, f->vol.file_start_lba + blk) != 1)
			return (copied > 0) ? copied : -1;

		memcpy (buffer + copied, f->buf + off, (size_t) chunk);
		f->filePos += chunk;
		copied += chunk;
		}

	return copied;
	}

static int
b_write_volume (b_io_fd fd, const char *buffer, int count)
	{
	b_fcb *f = &fcbArray[fd];
	uint64_t bs = f->vol.fs_block_size;
	uint64_t cap = vol_capacity_bytes (f);
	int written = 0;

	if (count <= 0)
		return 0;
	if ((f->flags & O_ACCMODE) == O_RDONLY)
		return -1;
	if (bs == 0 || f->vol.file_block_count == 0)
		return -1;
	if (f->flags & O_APPEND)
		f->filePos = (f->vol_file_size > (uint64_t) INT_MAX)
		                 ? INT_MAX
		                 : (int) f->vol_file_size;

	while (written < count)
		{
		uint64_t filePos = (uint64_t) f->filePos;
		uint64_t blk = filePos / bs;
		uint64_t off = filePos % bs;
		uint64_t remainBlk = bs - off;
		int need = count - written;
		int chunk = need;

		if (filePos >= cap)
			break;
		if (blk >= f->vol.file_block_count)
			break;
		if ((uint64_t) chunk > remainBlk)
			chunk = (int) remainBlk;
		if (filePos + (uint64_t) chunk > cap)
			chunk = (int) (cap - filePos);

		if (LBAread (f->buf, 1, f->vol.file_start_lba + blk) != 1)
			return (written > 0) ? written : -1;

		memcpy (f->buf + off, buffer + written, (size_t) chunk);
		if (LBAwrite (f->buf, 1, f->vol.file_start_lba + blk) != 1)
			return (written > 0) ? written : -1;

		f->filePos += chunk;
		written += chunk;
		if ((uint64_t) f->filePos > f->vol_file_size)
			f->vol_file_size = (uint64_t) f->filePos;
		}

	f->fileSize = (f->vol_file_size > (uint64_t) INT_MAX)
	                  ? INT_MAX
	                  : (int) f->vol_file_size;
	return written;
	}

void b_init ()
	{
	for (int i = 0; i < MAXFCBS; i++)
		{
		fcbArray[i].buf = NULL;
		}
	startup = 1;
	}

b_io_fd b_getFCB ()
	{
	for (int i = 0; i < MAXFCBS; i++)
		{
		if (fcbArray[i].buf == NULL)
			return i;
		}
	return (-1);
	}

b_io_fd b_open (char * filename, int flags)
	{
	b_io_fd returnFd;

	if (startup == 0)
		b_init ();

	returnFd = b_getFCB ();

	if (returnFd < 0)
		return -1;

	mfs_b_open_ctx ctx;
	memset (&ctx, 0, sizeof (ctx));

	if (mfs_volume_open (filename, flags, &ctx) == 0)
		{
		size_t bsz = (ctx.fs_block_size > 0) ? (size_t) ctx.fs_block_size
		                                     : (size_t) BLOCK_SIZE;
		char *newbuf = malloc (bsz);
		if (newbuf == NULL)
			{
			free (ctx.parent_dir);
			return -1;
			}

		fcbArray[returnFd].vol_open = 1;
		fcbArray[returnFd].vol = ctx;
		fcbArray[returnFd].fd = -1;
		fcbArray[returnFd].buf = newbuf;
		fcbArray[returnFd].index = 0;
		fcbArray[returnFd].buflen = 0;
		fcbArray[returnFd].filePos = 0;
		fcbArray[returnFd].flags = flags;
		fcbArray[returnFd].vol_file_size = ctx.file_size;
		fcbArray[returnFd].fileSize
		    = (ctx.file_size > (uint64_t) INT_MAX) ? INT_MAX : (int) ctx.file_size;

		return returnFd;
		}

	fcbArray[returnFd].vol_open = 0;
	memset (&fcbArray[returnFd].vol, 0, sizeof (fcbArray[returnFd].vol));
	fcbArray[returnFd].vol_file_size = 0;
	int osfd = open (filename, flags, 0666);
	if (osfd < 0)
		return -1;

	fcbArray[returnFd].fd = osfd;
	fcbArray[returnFd].buf = malloc (BLOCK_SIZE);
	if (fcbArray[returnFd].buf == NULL)
		{
		close (osfd);
		return -1;
		}

	fcbArray[returnFd].index = 0;
	fcbArray[returnFd].buflen = 0;
	fcbArray[returnFd].filePos = 0;
	fcbArray[returnFd].flags = flags;

	struct stat st;
	if (fstat (osfd, &st) == 0)
		fcbArray[returnFd].fileSize = (int) st.st_size;
	else
		fcbArray[returnFd].fileSize = 0;

	return (returnFd);
	}

int b_seek (b_io_fd fd, off_t offset, int whence)
	{
	if (startup == 0)
		b_init ();

	if ((fd < 0) || (fd >= MAXFCBS))
		return (-1);

	long long pos;

	if (whence == SEEK_SET)
		pos = (long long) offset;
	else if (whence == SEEK_CUR)
		pos = (long long) fcbArray[fd].filePos + (long long) offset;
	else if (whence == SEEK_END)
		{
		long long base = fcbArray[fd].vol_open
		                     ? (long long) fcbArray[fd].vol_file_size
		                     : (long long) fcbArray[fd].fileSize;
		pos = base + (long long) offset;
		}
	else
		return -1;

	if (pos < 0)
		pos = 0;

	if (fcbArray[fd].vol_open)
		{
		uint64_t cap = vol_capacity_bytes (&fcbArray[fd]);
		if ((uint64_t) pos > cap)
			pos = (long long) cap;
		}

	fcbArray[fd].filePos = (pos > INT_MAX) ? INT_MAX : (int) pos;

	if (!fcbArray[fd].vol_open)
		lseek (fcbArray[fd].fd, fcbArray[fd].filePos, SEEK_SET);

	vol_reset_buf_cursor (&fcbArray[fd]);

	return (fcbArray[fd].filePos);
	}

int b_write (b_io_fd fd, char * buffer, int count)
	{
	if (startup == 0)
		b_init ();

	if ((fd < 0) || (fd >= MAXFCBS))
		return (-1);

	if (fcbArray[fd].vol_open)
		return b_write_volume (fd, buffer, count);

	int bytesWritten = write (fcbArray[fd].fd, buffer, count);

	if (bytesWritten > 0)
		fcbArray[fd].filePos += bytesWritten;

	return (bytesWritten);
	}

int b_read (b_io_fd fd, char * buffer, int count)
	{
	if (startup == 0)
		b_init ();

	if ((fd < 0) || (fd >= MAXFCBS))
		return (-1);
	/* Route volume-backed files to our LBAread-based helper above.
 	* When b_open() found the file inside our filesystem, it set
 	* vol_open = 1. In that case fd == -1, so calling read(fd, ...)
 	* would fail — we must use LBAread through b_read_volume instead. */
	if (fcbArray[fd].vol_open)
		return b_read_volume (fd, buffer, count);

	int bytesCopied = 0;

	int available = fcbArray[fd].buflen - fcbArray[fd].index;

	if (available > 0)
		{
		int toCopy = (count < available) ? count : available;

		memcpy (buffer, fcbArray[fd].buf + fcbArray[fd].index, toCopy);

		fcbArray[fd].index += toCopy;
		bytesCopied += toCopy;
		}

	int remaining = count - bytesCopied;

	if (remaining >= BLOCK_SIZE)
		{
		int blocks = remaining / BLOCK_SIZE;
		int bytesToRead = blocks * BLOCK_SIZE;

		int readBytes
		    = read (fcbArray[fd].fd, buffer + bytesCopied, bytesToRead);

		bytesCopied += readBytes;
		}

	remaining = count - bytesCopied;

	if (remaining > 0)
		{
		int bytes = read (fcbArray[fd].fd, fcbArray[fd].buf, BLOCK_SIZE);

		if (bytes > 0)
			{
			fcbArray[fd].buflen = bytes;
			fcbArray[fd].index = 0;

			int toCopy = (remaining < bytes) ? remaining : bytes;

			memcpy (buffer + bytesCopied, fcbArray[fd].buf, toCopy);

			fcbArray[fd].index += toCopy;
			bytesCopied += toCopy;
			}
		}

	return (bytesCopied);
	}

int b_close (b_io_fd fd)
	{
	if ((fd < 0) || (fd >= MAXFCBS))
		return -1;

	if (fcbArray[fd].vol_open)
		{
		mfs_volume_close (&fcbArray[fd].vol, fcbArray[fd].vol_file_size);
		fcbArray[fd].vol_open = 0;
		memset (&fcbArray[fd].vol, 0, sizeof (fcbArray[fd].vol));
		}
	else if (fcbArray[fd].fd >= 0)
		close (fcbArray[fd].fd);

	free (fcbArray[fd].buf);
	fcbArray[fd].buf = NULL;
	vol_reset_buf_cursor (&fcbArray[fd]);

	return (0);
	}
