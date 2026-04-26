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
#include <errno.h>
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
b_write_volume (b_fcb *f, const char *buffer, int count)
	{
	if (count <= 0)
		return 0;

	if ((f->flags & O_ACCMODE) == O_RDONLY)
		{
		errno = EBADF;
		return -1;
		}

	uint64_t blockSize = f->vol.fs_block_size;
	uint64_t maxBytes = vol_capacity_bytes (f);
	if (blockSize == 0 || maxBytes == 0)
		{
		errno = ENOSPC;
		return -1;
		}

	if (f->flags & O_APPEND)
		f->filePos = (f->vol_file_size > (uint64_t) INT_MAX)
		                 ? INT_MAX
		                 : (int) f->vol_file_size;

	if ((uint64_t) f->filePos >= maxBytes)
		{
		errno = ENOSPC;
		return -1;
		}

	int totalWritten = 0;
	while (totalWritten < count)
		{
		uint64_t curPos = (uint64_t) f->filePos;
		if (curPos >= maxBytes)
			{
			if (totalWritten == 0)
				errno = ENOSPC;
			break;
			}

		uint64_t blockIndex = curPos / blockSize;
		uint64_t blockOffset = curPos % blockSize;
		uint64_t roomInBlock = blockSize - blockOffset;
		uint64_t remainingCap = maxBytes - curPos;
		int chunk = count - totalWritten;

		if ((uint64_t) chunk > roomInBlock)
			chunk = (int) roomInBlock;
		if ((uint64_t) chunk > remainingCap)
			chunk = (int) remainingCap;

		uint64_t lba = f->vol.file_start_lba + blockIndex;
		if (LBAread (f->buf, 1, lba) != 1)
			{
			errno = EIO;
			return (totalWritten > 0) ? totalWritten : -1;
			}

		memcpy (f->buf + blockOffset, buffer + totalWritten, (size_t) chunk);

		if (LBAwrite (f->buf, 1, lba) != 1)
			{
			errno = EIO;
			return (totalWritten > 0) ? totalWritten : -1;
			}

		f->filePos += chunk;
		totalWritten += chunk;
		}

	if (f->vol_file_size < (uint64_t) f->filePos)
		f->vol_file_size = (uint64_t) f->filePos;

	f->fileSize = (f->vol_file_size > (uint64_t) INT_MAX)
	                  ? INT_MAX
	                  : (int) f->vol_file_size;

	/* Keep read-buffer state consistent after volume writes. */
	vol_reset_buf_cursor (f);
	return totalWritten;
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
		return b_write_volume (&fcbArray[fd], buffer, count);

	int bytesWritten = write (fcbArray[fd].fd, buffer, count);

	if (bytesWritten > 0)
		fcbArray[fd].filePos += bytesWritten;

	return (bytesWritten);
	}
/* ---------------------------------------------------------------
 * b_read_volume
 *
 * This function handles reading from a file that lives inside OUR
 * filesystem volume (not a regular Linux file). Instead of using
 * the Linux read() system call, we use LBAread() to talk directly
 * to the disk blocks where our file's data is stored.
 *
 * Parameters:
 *   f      - pointer to the file control block (tracks position,
 *            buffer state, and which disk blocks belong to this file)
 *   buffer - the caller's buffer where we copy the data into
 *   count  - how many bytes the caller wants to read
 *
 * Returns the number of bytes actually copied, or -1 on error.
 * --------------------------------------------------------------- */
static int
b_read_volume (b_fcb *f, char *buffer, int count)
    {
    if (count <= 0)
        return 0;

    /* --- Clamp count to what's actually left in the file ---
     * vol_file_size is the real size of the file in bytes.
     * filePos is where we currently are in the file.
     * If we asked for more bytes than are left, shrink count
     * so we don't return garbage padding from the end of a block. */
    uint64_t remaining_in_file = (f->vol_file_size > (uint64_t) f->filePos)
                                     ? f->vol_file_size - (uint64_t) f->filePos
                                     : 0;
    if (remaining_in_file == 0)
        return 0;   /* Already at end of file, nothing to read */
    if ((uint64_t) count > remaining_in_file)
        count = (int) remaining_in_file;

    uint64_t blockSize = f->vol.fs_block_size;  /* e.g. 512 bytes per block */
    int bytesCopied = 0;                         /* running total of bytes copied so far */

    /* =============================================================
     * PHASE 1: Check if we already have leftover bytes in our buffer
     *
     * Our internal buffer (f->buf) can hold one block's worth of data.
     * f->buflen is how many bytes are valid in the buffer.
     * f->index is how far into the buffer we've already consumed.
     * So (buflen - index) = how many buffered bytes haven't been
     * handed to the caller yet.
     *
     * Example: last read buffered 512 bytes but the caller only
     * wanted 300, so 212 bytes are still sitting in f->buf waiting.
     * We serve those first before touching the disk again.
     * ============================================================= */
    int available = f->buflen - f->index;
    if (available > 0)
        {
        /* Don't copy more than the caller asked for */
        int toCopy = (count < available) ? count : available;
        memcpy (buffer, f->buf + f->index, toCopy);

        f->index    += toCopy;   /* advance our position inside the buffer */
        f->filePos  += toCopy;   /* advance our position inside the file */
        bytesCopied += toCopy;
        }

    int remaining = count - bytesCopied;  /* how many bytes we still owe the caller */

    /* =============================================================
     * PHASE 2: Read full blocks straight into the caller's buffer
     *
     * If the caller still wants at least one full block's worth of
     * data, we skip our internal buffer entirely and have LBAread()
     * write directly into the caller's buffer. This is the fast path
     * because it avoids an extra memcpy.
     *
     * We calculate which disk block to start at using:
     *   blockIndex = filePos / blockSize  (which block number within the file)
     *   lba = file_start_lba + blockIndex (actual disk address)
     * ============================================================= */
    if (remaining >= (int) blockSize)
        {
        /* How many complete blocks fit in 'remaining' bytes? */
        int blocks    = remaining / (int) blockSize;
        int wantBytes = blocks * (int) blockSize;

        /* Safety check: don't read past the end of the allocated disk space.
         * vol_capacity_bytes() returns file_block_count * block_size. */
        uint64_t cap = vol_capacity_bytes (f);
        if ((uint64_t) f->filePos + (uint64_t) wantBytes > cap)
            {
            uint64_t room = cap - (uint64_t) f->filePos;
            blocks    = (int) (room / blockSize);   /* fewer blocks fit */
            wantBytes = blocks * (int) blockSize;
            }

        if (blocks > 0)
            {
            /* Convert our file-relative block number to an absolute LBA */
            uint64_t blockIndex = (uint64_t) f->filePos / blockSize;
            uint64_t lba        = f->vol.file_start_lba + blockIndex;

            /* Read directly into the caller's buffer at the right offset */
            if (LBAread (buffer + bytesCopied, (uint64_t) blocks, lba)
                != (uint64_t) blocks)
                {
                errno = EIO;
                return (bytesCopied > 0) ? bytesCopied : -1;
                }

            f->filePos  += wantBytes;
            bytesCopied += wantBytes;
            }
        }

    remaining = count - bytesCopied;

    /* =============================================================
     * PHASE 3: Handle the leftover partial block at the end
     *
     * If the caller still wants bytes but less than a full block,
     * we have to read an entire block from disk into our internal
     * buffer (because LBAread always reads in whole blocks), then
     * copy just the bytes the caller needs out of it.
     *
     * The key insight: we keep the rest of the buffered block in
     * f->buf so that the NEXT call to b_read can use Phase 1
     * instead of hitting the disk again. This is what makes
     * reading byte-by-byte not ridiculously slow.
     * ============================================================= */
    if (remaining > 0)
        {
        /* Figure out which block filePos is currently inside,
         * and how far into that block we are */
        uint64_t blockIndex    = (uint64_t) f->filePos / blockSize;
        uint64_t offsetInBlock = (uint64_t) f->filePos % blockSize;
        uint64_t lba           = f->vol.file_start_lba + blockIndex;

        /* Read the whole block into our internal buffer */
        if (LBAread (f->buf, 1, lba) != 1)
            {
            errno = EIO;
            return (bytesCopied > 0) ? bytesCopied : -1;
            }

        /* How many bytes in this block are still part of our file?
         * (The rest are just block padding and don't belong to the file) */
        int bytesAvailInBlock = (int) blockSize - (int) offsetInBlock;
        if ((uint64_t) f->filePos + (uint64_t) bytesAvailInBlock
            > f->vol_file_size)
            bytesAvailInBlock
                = (int) (f->vol_file_size - (uint64_t) f->filePos);

        /* Update buffer metadata so Phase 1 works correctly next call:
         *   buflen = full block size (all bytes are now valid in f->buf)
         *   index  = where we are inside the buffer right now */
        f->buflen = (int) blockSize;
        f->index  = (int) offsetInBlock;

        int toCopy = (remaining < bytesAvailInBlock)
                         ? remaining
                         : bytesAvailInBlock;
        memcpy (buffer + bytesCopied, f->buf + offsetInBlock, toCopy);

        /* Advance both the buffer cursor and the file position */
        f->index    += toCopy;
        f->filePos  += toCopy;
        bytesCopied += toCopy;
        }

    return bytesCopied;
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
    	return b_read_volume (&fcbArray[fd], buffer, count);

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
