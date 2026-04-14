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
#include <stdlib.h>			// for malloc
#include <string.h>			// for memcpy
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "b_io.h"

#define MAXFCBS 20
#define BLOCK_SIZE 512   

typedef struct b_fcb
	{
	/** TODO add al the information you need in the file control block **/
	char * buf;		
	int index;		
	int buflen;		

	int fd;          
	int filePos;     
	int fileSize;    
	int flags;      

	} b_fcb;
	
b_fcb fcbArray[MAXFCBS];

int startup = 0;

//to initialize our file system
void b_init ()
	{
	for (int i = 0; i < MAXFCBS; i++)
		{
		fcbArray[i].buf = NULL;
		}
	startup = 1;
	}

//to get a free FCB element
b_io_fd b_getFCB ()
	{
	for (int i = 0; i < MAXFCBS; i++)
		{
		if (fcbArray[i].buf == NULL)
			{
			return i;
			}
		}
	return (-1);
	}
	
// Interface to open a buffered file
b_io_fd b_open (char * filename, int flags)
	{
	b_io_fd returnFd;

	if (startup == 0) b_init();
	
	returnFd = b_getFCB();

	if (returnFd < 0) return -1;

	int osfd = open(filename, flags, 0666);   
	if (osfd < 0) return -1;

	fcbArray[returnFd].fd = osfd;              
	fcbArray[returnFd].buf = malloc(BLOCK_SIZE); 
	fcbArray[returnFd].index = 0;
	fcbArray[returnFd].buflen = 0;
	fcbArray[returnFd].filePos = 0;
	fcbArray[returnFd].flags = flags;

	struct stat st;                         
	if (fstat(osfd, &st) == 0)
		fcbArray[returnFd].fileSize = st.st_size;
	else
		fcbArray[returnFd].fileSize = 0;
	
	return (returnFd);
	}


// Interface to seek function	
int b_seek (b_io_fd fd, off_t offset, int whence)
	{
	if (startup == 0) b_init();

	if ((fd < 0) || (fd >= MAXFCBS))
		{
		return (-1);
		}
		
	if (whence == SEEK_SET)
		fcbArray[fd].filePos = offset;
	else if (whence == SEEK_CUR)
		fcbArray[fd].filePos += offset;
	else if (whence == SEEK_END)
		fcbArray[fd].filePos = fcbArray[fd].fileSize + offset;

	lseek(fcbArray[fd].fd, fcbArray[fd].filePos, SEEK_SET);  

	fcbArray[fd].index = 0;   // reset buffer
	fcbArray[fd].buflen = 0;

	return (fcbArray[fd].filePos);
	}


// Interface to write function	
int b_write (b_io_fd fd, char * buffer, int count)
	{
	if (startup == 0) b_init();

	if ((fd < 0) || (fd >= MAXFCBS))
		{
		return (-1);
		}
		
	int bytesWritten = write(fcbArray[fd].fd, buffer, count); 

	if (bytesWritten > 0)
		fcbArray[fd].filePos += bytesWritten;

	return (bytesWritten);
	}


// Interface to read a buffer
int b_read (b_io_fd fd, char * buffer, int count)
	{

	if (startup == 0) b_init();

	if ((fd < 0) || (fd >= MAXFCBS))
		{
		return (-1);
		}

	int bytesCopied = 0;

	// Part 1: use existing buffer
	// 
	int available = fcbArray[fd].buflen - fcbArray[fd].index;

	if (available > 0)
	{
		int toCopy = (count < available) ? count : available;

		memcpy(buffer,
			   fcbArray[fd].buf + fcbArray[fd].index,
			   toCopy);

		fcbArray[fd].index += toCopy;
		bytesCopied += toCopy;
	}

	
	// Part 2: full block reads
	int remaining = count - bytesCopied;

	if (remaining >= BLOCK_SIZE)
	{
		int blocks = remaining / BLOCK_SIZE;
		int bytesToRead = blocks * BLOCK_SIZE;

		int readBytes = read(fcbArray[fd].fd,
							 buffer + bytesCopied,
							 bytesToRead);

		bytesCopied += readBytes;
	}

	// Part 3: leftover bytes
	remaining = count - bytesCopied;

	if (remaining > 0)
	{
		int bytes = read(fcbArray[fd].fd,
						 fcbArray[fd].buf,
						 BLOCK_SIZE);

		if (bytes > 0)
		{
			fcbArray[fd].buflen = bytes;
			fcbArray[fd].index = 0;

			int toCopy = (remaining < bytes) ? remaining : bytes;

			memcpy(buffer + bytesCopied,
				   fcbArray[fd].buf,
				   toCopy);

			fcbArray[fd].index += toCopy;
			bytesCopied += toCopy;
		}
	}
		
	return (bytesCopied);
	}
	
// Interface to Close the file	
int b_close (b_io_fd fd)
	{
	if ((fd < 0) || (fd >= MAXFCBS))
		return -1;

	close(fcbArray[fd].fd);       

	free(fcbArray[fd].buf);        
	fcbArray[fd].buf = NULL;

	return (0);
	}