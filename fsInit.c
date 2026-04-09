/**************************************************************
* Class::  CSC-415-0# Fall 2025
* Name:: Jiawei Lyu, Leslie Raya, Alexandra Borders, Yeraldin Crespo
* Student IDs:: 923809065, 921813630, 913630008, 923523819
* GitHub-Name:: jiaweilyu0929
* Group-Name:: Team #1
* Project:: Basic File System
*
* File:: fsInit.c
*
* Description:: Partition open/close integration with Phase 1
*   mount or format.
*
**************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fsLow.h"
#include <time.h>

#define FS_MAGIC                0x46534331u  /* 'FSC1' */
#define FS_VERSION              1u
#define FS_ROOT_DIR_BLOCKS      4
#define MAX_FILENAME            255
#define DIR_ENTRIES             50

/* The superblock is the first thing written to the volume and the first
 * thing read on startup. It stores the location of every other structure
 * so the file system knows where to find the bitmap and root directory. */
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
/* One directory entry represents one file or folder inside a directory.
 * Each entry tracks the name, location on disk, size, timestamps,
 * and whether the slot is currently in use or available. */
typedef struct __attribute__((packed)) {
        char name[MAX_FILENAME + 1];
        uint8_t  fileType;               
        uint8_t  inUse;                  
        uint64_t startBlock;             
        uint64_t blockCount;          
        uint64_t size;            
        time_t   createTime;       
        time_t   modifiedTime;           
        time_t   accessTime;             
} fs_dirent_t;

static void initRootDir(uint64_t root_dir_start, uint64_t blockSize);
static int fs_vol_format(uint64_t numberOfBlocks, uint64_t blockSize);

static fs_superblock_t g_fs_sb;
static int g_fs_mounted = 0;

static uint8_t *g_bitmap = NULL;
static uint64_t g_bitmap_blocks = 0;

/* Marks a single block as taken in the bitmap. Each bit in the bitmap
 * represents one block on disk — 0 means free, 1 means used.
 * This function flips the correct bit to 1. */
static void
bitmap_set_used (uint8_t *bitmap, uint64_t block_idx)
        {
        uint64_t byte_idx = block_idx / 8;
        uint64_t bit_idx = block_idx % 8;
        bitmap[byte_idx] |= (uint8_t) (1u << bit_idx);
        }
/* Saves the superblock to block 0 on disk. We write a full block at a
 * time because the disk only accepts block-sized writes. This ensures
 * our metadata is saved permanently even if the system restarts. */
static int
write_superblock (const fs_superblock_t *sb, uint64_t block_size)
        {
        uint8_t *buf = calloc (1, (size_t) block_size);
        if (buf == NULL)
                return -1;
        memcpy (buf, sb, sizeof (fs_superblock_t));
        if (LBAwrite (buf, 1, 0) != 1)
                {
                free (buf);
                return -1;
                }
        free (buf);
        return 0;
        }

/* Loads block 0 from disk into memory so we can inspect the superblock.
 * Called at startup to check whether the volume is already formatted
 * before deciding whether to format or just mount it. */
static int
read_superblock (fs_superblock_t *sb, uint64_t block_size)
        {
        uint8_t *buf = malloc ((size_t) block_size);
        if (buf == NULL)
                return -1;
        if (LBAread (buf, 1, 0) != 1)
                {
                free (buf);
                return -1;
                }
        memcpy (sb, buf, sizeof (fs_superblock_t));
        free (buf);
        return 0;
        }

/* Tries to use an existing formatted volume without reformatting it.
 * Reads the superblock and checks the magic number — if it matches,
 * the volume is valid and ready to use as-is. */static int
fs_vol_try_mount (uint64_t numberOfBlocks, uint64_t blockSize)
        {
        fs_superblock_t sb;

        if (read_superblock (&sb, blockSize) != 0)
                return -1;
        if (sb.magic != FS_MAGIC || sb.version != FS_VERSION)
                return 1;
        if (sb.block_size != blockSize || sb.total_blocks != numberOfBlocks)
                return -1;

        memcpy (&g_fs_sb, &sb, sizeof (fs_superblock_t));
        g_fs_mounted = 1;
        return 0;
        }

/* Sets up the root directory when the volume is first formatted.
 * Creates the "." entry (this directory) and ".." entry (parent directory).
 * Since root has no parent, both entries point to the root itself. */
static void
initRootDir(uint64_t root_dir_start, uint64_t blockSize)
        {
        uint64_t dirBytes      = FS_ROOT_DIR_BLOCKS * blockSize;
        int      actualEntries = (int)(dirBytes / sizeof(fs_dirent_t));
         fs_dirent_t *root      = calloc(FS_ROOT_DIR_BLOCKS, (size_t)blockSize);
         if (root == NULL)
        {
        fprintf(stderr, "initRootDir: calloc failed\n");
        return;
        }

        time_t now = time(NULL);

        strncpy(root[0].name, ".", MAX_FILENAME);
        root[0].fileType     = 2;
        root[0].inUse        = 1;
        root[0].startBlock   = root_dir_start;
        root[0].blockCount   = FS_ROOT_DIR_BLOCKS;
        root[0].size         = dirBytes;
        root[0].createTime   = now;
        root[0].modifiedTime = now;
        root[0].accessTime   = now;

        strncpy(root[1].name, "..", MAX_FILENAME);
        root[1].fileType     = 2;
        root[1].inUse        = 1;
        root[1].startBlock   = root_dir_start;
        root[1].blockCount   = FS_ROOT_DIR_BLOCKS;
        root[1].size         = dirBytes;
        root[1].createTime   = now;
        root[1].modifiedTime = now;
        root[1].accessTime   = now;

        LBAwrite(root, FS_ROOT_DIR_BLOCKS, root_dir_start);
        free(root);
        }
/* Finds and reserves a requested number of contiguous free blocks.
 * Scans the bitmap for enough consecutive free bits, marks them as used,
 * and writes the updated bitmap to disk so the change is saved. */
static int
allocateBlocks(uint64_t count)
        {
        if (g_bitmap == NULL)
                {
                fprintf(stderr, "allocateBlocks: bitmap not initialized\n");
                return -1;
                }

        uint64_t totalBlocks = g_fs_sb.total_blocks;
        uint64_t found       = 0;
        uint64_t startBlock  = 0;

        for (uint64_t i = 0; i < totalBlocks; i++)
                {
                uint64_t byte_idx = i / 8;
                uint64_t bit_idx  = i % 8;

                if (!(g_bitmap[byte_idx] & (1u << bit_idx)))
                        {
                        if (found == 0) startBlock = i;
                        found++;
                        if (found == count)
                                {
                                for (uint64_t j = startBlock; j < startBlock + count; j++)
                                        {
                                        g_bitmap[j / 8] |= (1u << (j % 8));
                                        }
                                g_fs_sb.free_block_count -= count;
                                LBAwrite(g_bitmap, g_bitmap_blocks,
                                         g_fs_sb.bitmap_start_lba);
                                return (int)startBlock;
                                }
                        }
                else
                        {
                        found = 0;
                        }
                }

        fprintf(stderr, "allocateBlocks: not enough contiguous free blocks\n");
        return -1;
        }

/* Formats the volume from scratch when no valid file system is found.
 * Sets up the bitmap to track free space, initializes the root directory,
 * and writes the superblock last to confirm everything was successful. */
static int
fs_vol_format (uint64_t numberOfBlocks, uint64_t blockSize)
        {
        fs_superblock_t sb;
        uint64_t bitmap_bytes;
        uint64_t bitmap_block_count;
        uint64_t root_dir_start;
        uint64_t first_data_lba;
        uint8_t *bitmap = NULL;
        uint64_t i;

        memset (&sb, 0, sizeof (sb));
        if (numberOfBlocks < 8 || blockSize < 64)
                return -1;

        bitmap_bytes = (numberOfBlocks + 7) / 8;
        bitmap_block_count = (bitmap_bytes + blockSize - 1) / blockSize;
        root_dir_start = 1 + bitmap_block_count;
        first_data_lba = root_dir_start + FS_ROOT_DIR_BLOCKS;

        if (first_data_lba > numberOfBlocks)
                {
                fprintf (stderr, "fs_vol_format: volume too small for metadata\n");
                return -1;
                }

        bitmap = calloc (1, (size_t) (bitmap_block_count * blockSize));
        if (bitmap == NULL)
                return -1;

        for (i = 0; i < first_data_lba; i++)
                bitmap_set_used (bitmap, i);

        {
        uint64_t total_bits = bitmap_block_count * blockSize * 8;
        for (i = numberOfBlocks; i < total_bits; i++)
                bitmap_set_used (bitmap, i);
        }
        g_bitmap = bitmap;
        g_bitmap_blocks = bitmap_block_count;

        sb.magic = FS_MAGIC;
        sb.version = FS_VERSION;
        sb.block_size = blockSize;
        sb.total_blocks = numberOfBlocks;
        sb.free_block_count = numberOfBlocks - first_data_lba;
        sb.bitmap_start_lba = 1;
        sb.bitmap_block_count = bitmap_block_count;
        sb.root_dir_start_lba = root_dir_start;
        sb.root_dir_block_count = FS_ROOT_DIR_BLOCKS;
        sb.first_data_lba = first_data_lba;

        if (write_superblock (&sb, blockSize) != 0)
                {
                free (bitmap);
                return -1;
                }

        if (LBAwrite (bitmap, bitmap_block_count, sb.bitmap_start_lba)
            != bitmap_block_count)
                {
                free (bitmap);
                return -1;
                }
        
        initRootDir(root_dir_start, blockSize);

        memcpy (&g_fs_sb, &sb, sizeof (fs_superblock_t));
        g_fs_mounted = 1;
        printf ("Volume formatted: %llu blocks, %llu bytes/block, "
                "data starts at LBA %llu\n",
                (unsigned long long) numberOfBlocks,
                (unsigned long long) blockSize,
                (unsigned long long) first_data_lba);
        return 0;
        }
/* The main entry point called when the file system starts up.
 * First checks if the volume is already formatted and tries to mount it.
 * If not, it formats the volume fresh before returning. */
int 
initFileSystem (uint64_t numberOfBlocks, uint64_t blockSize)
	{
	int r;

	printf ("Initializing File System with %llu blocks with a block "
		"size of %llu\n",
		(unsigned long long) numberOfBlocks,
		(unsigned long long) blockSize);

	r = fs_vol_try_mount (numberOfBlocks, blockSize);
	if (r == 1)
		{
		printf ("No valid filesystem found; formatting...\n");
		r = fs_vol_format (numberOfBlocks, blockSize);
		}
	if (r != 0)
		{
		g_fs_mounted = 0;
		return -1;
		}
	printf ("File system ready.\n");
	return 0;
	}

/* Called when the file system shuts down cleanly.
 * Marks the volume as unmounted. A full implementation would also
 * flush any unsaved data to disk before closing. */
void
exitFileSystem (void)
	{
	printf ("System exiting\n");
	g_fs_mounted = 0;
	}
