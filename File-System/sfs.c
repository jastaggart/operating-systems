#include <stdio.h>
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include "disk_emu.h"
#include "sfs_api.h"

/*
Name: Jasmine Taggart
ID: 261056534

NOTE: include -lm in Make file to use ceil() and floor()
max number of files = 100
man file name length = 16
number of available data blocks = 4096 - 3 for directory, 4093 for files + block pointers

disk structure: 
 block 0 - super node
 blocks 1 to 6 - i-node table
 blocks 7 to 4102 - data blocks
   - 7 to 9 = directory
   - 10 to 4102 = file data blocks
 blocks 4103 to 4106 - free bit map

 num inodes per inode block = 18.3
*/
char *disk_file = "sfs_disk";
int DISK_BLOCK_SIZE = 1024;
int NUM_BLOCKS = 4107;
int FBM_BLOCK = 4103;
int INODE_BLOCK = 1;
int DATA_BLOCK = 7; // first 3 data blocks are root dir
int ROOT_INODE = 0;

char free_bit_map[4096];

struct dir_entry {
	char filename[17];
	int inode;
	bool occupied;
} directory[100]; // each entry = 21B

struct opened_file {
	int inode;
	int fp; //read/write ptr
	bool open;
} fdt[100];

struct inode {
	bool occupied;
	int filesize; // in bytes
	int direct_ptr[12];
	int indirect_ptr;
} inode_table[101];

struct super_block {
	int magic;
	int block_size;
	int sfs_size;
	int inode_table_length;
	int root_inode;
};

int get_next_file_num = 0;

void mksfs(int fresh) {
	if (!fresh) {
		// 1. load existing disk - if unsuccessful, exit
		if (init_disk(disk_file, DISK_BLOCK_SIZE, NUM_BLOCKS) != 0) {
			perror("init_disk error");
			exit(1);
		}
		
		// 2. cache inode table
		read_blocks(INODE_BLOCK, 6, inode_table);
		
		// 3. cache root directory
		int dir_size = inode_table[0].filesize; // num bytes in directory
		int numDirBlocks = (int) ceil(dir_size/3.0); // num blocks to read for dir

		// read directory data blocks
		char *dir_blocks = malloc(numDirBlocks*DISK_BLOCK_SIZE);
		for (int i = 0; i < numDirBlocks; i++) {
			read_blocks(inode_table[0].direct_ptr[i], 1, dir_blocks + DISK_BLOCK_SIZE*i);
		}
		
		// copy 21B of directory data blocks at a time into the corresponding file entry in dir table
		// (each entry is 21B)
		for (int i = 0; i < dir_size/21; i++) {
			memcpy(directory[i].filename, dir_blocks + i*21, 16);
			
			int* inode = (int *) malloc(sizeof(int));
			memcpy(inode, dir_blocks + i*21 +16, 4);
			directory[i].inode = *inode;
			
			bool* occupied = (bool *) malloc(sizeof(bool));
			memcpy(occupied, dir_blocks + i*21 +20, 1);
			directory[i].occupied = *occupied;

			free(inode);
			free(occupied);
		}

		// 4. cache free bit map
		read_blocks(FBM_BLOCK, 4, free_bit_map);

		// 5. free unneeded allocated memory
		free(dir_blocks);
	}
	else {
		// 1. initialize disk - if unsuccessful, exit
		if (init_fresh_disk(disk_file, DISK_BLOCK_SIZE, NUM_BLOCKS) != 0) {
			perror("init_fresh_disk error");
			exit(1);
		}

		// 2. set up free bit map (cached in memory)
		for (int i = 0; i < DISK_BLOCK_SIZE*4; i++) { // first 3 are for directory so leave those bytes as 0
			if (i < 3) {
				free_bit_map[i] = '0';
			}
			else free_bit_map[i] = '1';
		}
		write_blocks(FBM_BLOCK, 4, free_bit_map); // write fbm to disk using 4 blocks
		
		// 3. set up empty root directory on disk
		write_blocks(DATA_BLOCK, 3, directory); // put directory in first 3 data blocks
		
		// 4. create i node table + root dir i node
		inode_table[0].occupied = true;
		inode_table[0].filesize = 0;
		inode_table[0].direct_ptr[0] = DATA_BLOCK;
		inode_table[0].direct_ptr[1] = DATA_BLOCK + 1;
		inode_table[0].direct_ptr[2] = DATA_BLOCK + 2;
		write_blocks(INODE_BLOCK, 6, inode_table);
		
		// 5. set up super block on disk
		struct super_block *superblock = (struct super_block *) calloc(1, sizeof(struct super_block));
		superblock->magic = 1;
		superblock->block_size = 1024;
		superblock->sfs_size = 4107;
		superblock->inode_table_length = 6;
		superblock->root_inode = 0; // directory is the first i-node in i-node table
		write_blocks(0, 1, superblock);
		free(superblock);
	}
}

int sfs_fopen(char *fname) {
	// first check if file name is too long
	if (strlen(fname) > 16) {
		printf("sfs_fopen error: file name %s is too long\n", fname);
		return -1;
	}
	// 1. search directory for file
	int f_inode = -1;
	int fd = -1;

	for (int i = 0; i < 100; i++) {
		// 2. if file is found, check if file is already opened (if it is, return its fd)
		if (strcmp(directory[i].filename, fname) == 0) { 
			f_inode = directory[i].inode;
			for (int j = 0; j < 100; j++) {
				if (fdt[j].inode == f_inode && fdt[j].open) {
					fd = j;
					break;
				}
			}
			// if file is not opened, find next empty slot in fdt and add file to it
			if (fd == -1) {
				for (int j = 0; j < 100; j++) {
					if (!fdt[j].open) {
						fdt[j].inode = f_inode;
						fdt[j].fp = inode_table[f_inode].filesize;
						fdt[j].open = true;
						fd = j;
						break;
					}
				}
			}
			if (fd == -1) {
				printf("sfs_fopen: no fdt slot found\n");
				return -1;
			}
			break;
		}
	}

	// 3. if file is not found, create a new file and add it to fdt
	if (f_inode == -1) {
		// find next available slot in inode table and create inode for new file
		for (int i = 0; i < 101; i++) {
			if (!inode_table[i].occupied) {
				f_inode = i;
				inode_table[i].occupied = true;
				inode_table[i].filesize = 0;
				break;
			}
		}
		// if no available slot was found in inode table, print error
		if (f_inode == -1) {
			printf("sfs_fopen error: failed to create file %s, inode table is full.\n", fname);
			return -1;
		}

		// create directory entry
		int entry = -1;
		for (int i = 0; i < 100; i++) {
			if (!directory[i].occupied) {
				entry = i;
				strcpy(directory[i].filename, fname);
				directory[i].inode = f_inode;
				directory[i].occupied = true;
				break;
			}
		}
		// if no available slot was found in directory table, print error
		if (entry == -1) {
			printf("sfs_fopen error: failed to create file %s, directory table is full.\n", fname);
			return -1;
		}

		// add file to fdt
		for (int i = 0; i < 100; i++) {
			if (!fdt[i].open) {
				fdt[i].inode = f_inode;
				fdt[i].fp = 0;
				fdt[i].open = true;
				fd = i;
				break;
			}
		}
		if (fd == -1) {
				printf("sfs_fopen: no fdt slot found\n");
				return -1;
			}

		// update disk (directory + inode)
		write_blocks(DATA_BLOCK, 3, directory); // write directory to disk
		write_blocks(INODE_BLOCK, 6, inode_table); // write inode table to disk
	}
	return fd;
}

int sfs_fclose(int fileID) {
	if (fileID > 99 || !fdt[fileID].open) {
		printf("sfs_fclose: file id %d is not open\n", fileID);
		return -1;
	}
	fdt[fileID].open = false;
	return 0;
}

int sfs_remove(char* fname) {
	int dir_entry = -1;
	int inode = -1;
	int index_block[256];
	
	// 1. search for file in directory
	for (int i = 0; i < 100; i++) {
			if (strcmp(directory[i].filename, fname) == 0) {
				// if file is found in dir then make sure it is closed before removing it
				for (int j = 0; j < 100; j++) {
					if (fdt[j].inode == directory[i].inode && fdt[j].open) {
						printf("sfs_remove error: file %s is still open.\n", fname);
						return -1;
					}
				}
				dir_entry = i;
				inode = directory[i].inode;

				// 2. free the data blocks associated to file in fbm
				int numPtrs = (int) ceil((double) inode_table[inode].filesize/DISK_BLOCK_SIZE); // get number of blocks the file uses

				// if file used index block to point to data blocks, cache index block and free it
				if (numPtrs > 12) {
					read_blocks(inode_table[inode].indirect_ptr, 1, index_block);
					free_bit_map[inode_table[inode].indirect_ptr] = '0';
				} 

				for (int j = 0; j < numPtrs; j++) {
					if (j < 12) {
						free_bit_map[inode_table[inode].direct_ptr[j]] = '0';
					}
					else {
						free_bit_map[index_block[j - 12]] = '0';
					}
				}

				// 3. set entry's occupied flag to false so that entry slot can be reused
				directory[i].occupied = false;
				break;
			}
	}
	if (dir_entry == -1) {
		printf("sfs_remove error: file %s not found.\n", fname);
		return -1;
	}

	// remove inode entry 
	inode_table[inode].occupied = false;

	// update disk (fbm + inode + directory)
	write_blocks(FBM_BLOCK, 4, free_bit_map); // write fbm to disk using 4 blocks
	write_blocks(DATA_BLOCK, 3, directory); // write directory to disk
	write_blocks(INODE_BLOCK, 6, inode_table); // write inode table to disk
	return 0;
}

int sfs_fwrite(int fileID, const char* buffer, int length) {
	// check if file is open. if not, return 0
	if (!fdt[fileID].open) {
		printf("sfs_fwrite: file not open\n");
		return 0;
	}

	// define variables needed to determine number of blocks to allocate on disk and which data blocks to write to
	int inode = fdt[fileID].inode;
	int start_byte = fdt[fileID].fp;
	int end_byte = start_byte + length - 1;
	// printf("start byte: %d\n", start_byte);
	// printf("end byte: %d\n", end_byte);

	// check if file is full
	if (start_byte >= 268*DISK_BLOCK_SIZE) {
		puts("sfs_fwrite: file is full");
		return 0;
	}
	// check if write exceeds max number of data blocks a file can have (268) and reduce write size if necessary
	if (end_byte >= 268*DISK_BLOCK_SIZE) {
		end_byte = 268*DISK_BLOCK_SIZE - 1;
		length = end_byte - start_byte + 1;
	}

	int last_block = (int) ceil((double) inode_table[inode].filesize/DISK_BLOCK_SIZE) - 1; // the last file block allocated for this file before the write
	int startw_block = (int) floor((double) start_byte/DISK_BLOCK_SIZE); // file block that our write starts in
	int endw_block = (int) floor((double) end_byte/DISK_BLOCK_SIZE); // file block that our write ends in
	int num_new_blocks = endw_block - last_block; //number of new data blocks to allocate for this write
	int new_block_num = -1; // data block number obtained from fbm for each new block for this file
	int index_block[256]; // cache for the index block, if necessary to retrieve it
	
	// cache index block if pointers will be needed beyond the 12 direct ptrs
	if (endw_block > 11) {
		// if indirect ptr hasn't been used yet, find a free block for index block in fbm
		if (inode_table[inode].indirect_ptr == 0) {
			for (int i = 0; i < 4096; i++) {
				if (free_bit_map[i] == '1') {
					free_bit_map[i] = '0';
					inode_table[inode].indirect_ptr = i + DATA_BLOCK;
					break;
				}
			}
			if (inode_table[inode].indirect_ptr == 0) {
				printf("sfs_fwrite: no space to allocate to index block\n");
				return -1;
			}
		}
		read_blocks(inode_table[inode].indirect_ptr, 1, index_block);
	}

	// allocate new blocks needed for write and assign an inode pointer to each block
	for (int i = 1; i <= num_new_blocks; i++) {
		// search fbm for a free data block to allocate
		for (int j = 0; j < 4096; j++) {
			if (free_bit_map[j] == '1') {
				free_bit_map[j] = '0';
				new_block_num = j + DATA_BLOCK;
				break;
			}
		}
		// check if a free block was found
		if (new_block_num == -1) {
			endw_block = last_block + i - 1;
			end_byte = (endw_block + 1)*1024 - 1;

			if (endw_block < startw_block) {
				printf("sfs_fwrite: not enough space to write any bytes\n");
				return 0;
			}
			break;
		}

		// if free block was found, add it to file's inode 
		int cur_block = last_block + i; // the file block number whose pointer to update

		// check if block ptr to update is part of direct pointer array or indirect pointer's index block
		if (cur_block < 12) {
			inode_table[inode].direct_ptr[cur_block] = new_block_num;
		}
		else {
			index_block[cur_block - 12] = new_block_num;
		}
		new_block_num = -1;
	}

	// all necessary data blocks for write are allocated, so begin writing:
	int bytes_written = 0;
	int start_position = start_byte % DISK_BLOCK_SIZE; // writing start position in block to write in
	int bytes_to_write; // number of bytes to write in a specific block
	int block_num; // number of data block to write in
	char* temp_buf = (char *) malloc(DISK_BLOCK_SIZE*(sizeof(char)));

	// check if we need to append to a previously allocated block
	if (startw_block == last_block) {
		if (startw_block < 12) {
			block_num = inode_table[inode].direct_ptr[startw_block];
		}
		else {
			block_num = index_block[startw_block - 12];
		}
		read_blocks(block_num, 1, temp_buf);

		if (DISK_BLOCK_SIZE - start_position < length - bytes_written) {
			bytes_to_write = DISK_BLOCK_SIZE - start_position;
		}
		else {
			bytes_to_write = length - bytes_written;
		}
		
		memcpy(temp_buf + start_position, buffer, bytes_to_write);
		startw_block++;
		bytes_written += bytes_to_write;
		fdt[fileID].fp += bytes_to_write;
		write_blocks(block_num, 1, temp_buf);
	}

	// write to the newly allocated blocks
	for (int i = startw_block; i <= endw_block; i++) {
		// calc num bytes to write in block i 
		if ((length - bytes_written) <= DISK_BLOCK_SIZE) {
			bytes_to_write = length - bytes_written;
		}
		else bytes_to_write = DISK_BLOCK_SIZE;

		memcpy(temp_buf, buffer + bytes_written, bytes_to_write);
		if (i < 12) {
			write_blocks(inode_table[inode].direct_ptr[i], 1, temp_buf);
		}
		else {
			write_blocks(index_block[i - 12], 1, temp_buf);
		}
		bytes_written += bytes_to_write;
		fdt[fileID].fp += bytes_to_write;
	}
	// update file size 
	if (inode_table[inode].filesize < end_byte + 1) {
		inode_table[inode].filesize = end_byte + 1;
	}

	// update inode in disk
	write_blocks(INODE_BLOCK, 6, inode_table);

	// update index block in disk
	write_blocks(inode_table[inode].indirect_ptr, 1, index_block);

	// update fbm in disk
	write_blocks(FBM_BLOCK, 4, free_bit_map);

	return bytes_written;
}

int sfs_fread(int fileID, char* buffer, int length) {
	// check if file is open. if not, return 0
	if (!fdt[fileID].open) {
		printf("sfs_fread: file not open\n");
		return 0;
	}
	int inode = fdt[fileID].inode;

	// check if fp is out of bounds
	if (inode_table[inode].filesize <= fdt[fileID].fp) {
		printf("sfs_fread: read is out of file bounds\n");
		return 0;
	}

	// check if read goes out of bounds. if it does, edit the length to be in bounds
	if (fdt[fileID].fp + length > inode_table[inode].filesize) {
		length = inode_table[inode].filesize - fdt[fileID].fp; // num bytes to read is everything from fp to end of file
	}

	// determine data block numbers to read from disk
	char* temp_buf = (char *) malloc(DISK_BLOCK_SIZE*(sizeof(char))); // buffer to read a data block into
	int bytes_left = length;
	int start_block = (int) floor((double) fdt[fileID].fp/DISK_BLOCK_SIZE); // calculate which file block the fp is in
	int end_block = (int) floor((double) (fdt[fileID].fp + length - 1)/DISK_BLOCK_SIZE); // calculate which file block the read ends in

	int index_block[256];
	if (end_block > 11) {	
		read_blocks(inode_table[inode].indirect_ptr, 1, index_block);
	}

	// if read starts somewhere in a block
	if (fdt[fileID].fp % DISK_BLOCK_SIZE != 0) {
		// if file block # < 12, read data block pointed to by direct pointer (direct pointers are file blocks 0-11)
		if (start_block < 12) {
			read_blocks(inode_table[inode].direct_ptr[start_block], 1, temp_buf);
		}
		
		// if file block # >= 12, read data block from index block 
		else {
			read_blocks(index_block[start_block-12], 1, temp_buf);
		}
		// if num bytes available to read in block is less than the num bytes left to read, then
		// read all the bytes available in block and decrement bytes left
		if (DISK_BLOCK_SIZE - fdt[fileID].fp % DISK_BLOCK_SIZE < bytes_left) {
			memcpy(buffer, temp_buf + fdt[fileID].fp % DISK_BLOCK_SIZE, DISK_BLOCK_SIZE - fdt[fileID].fp % DISK_BLOCK_SIZE);
			bytes_left -= DISK_BLOCK_SIZE - fdt[fileID].fp % DISK_BLOCK_SIZE;
		}
		// if not, then just read num bytes left to read
		else {
			memcpy(buffer, temp_buf + fdt[fileID].fp % DISK_BLOCK_SIZE, bytes_left);
			bytes_left = 0;
		}
		start_block++;
	}

	// read data blocks into buffer
	for (int i = start_block; i <= end_block; i++) {
		// if file block # < 12, read data block pointed to by direct pointer (direct pointers are file blocks 0-11)
		if (i < 12) {
			read_blocks(inode_table[inode].direct_ptr[i], 1, temp_buf);
		}
		
		// if file block # >= 12, read data block from index block 
		else {
			read_blocks(index_block[i-12], 1, temp_buf);
		}

		//copy num of bytes needed from data block into buffer 
		if (bytes_left <= DISK_BLOCK_SIZE) {
			memcpy(buffer + length - bytes_left, temp_buf, bytes_left);
			bytes_left = 0; // read is complete
		}
		else {
			memcpy(buffer + length - bytes_left, temp_buf, DISK_BLOCK_SIZE); // copy whole block into buffer
			bytes_left -= DISK_BLOCK_SIZE; // decrement bytes remaining by block size
		}
	}
	free(temp_buf);
	fdt[fileID].fp += length;
	return length;
}

int sfs_fseek(int fileID, int location) {
	 if (!fdt[fileID].open) {
	 	printf("sfs_fseek error: file with id %d is not open.\n", fileID);
	 	return -1;
	 }
	 fdt[fileID].fp = location;
	 return 0;
}

int sfs_getnextfilename(char* fname) {
	// if next entry in directory is empty, return 0
	if (!directory[get_next_file_num].occupied) {
		return 0;
	}
	// get next directory entry, copy next file name into buffer, increment counter
	char* found_file = directory[get_next_file_num].filename;
	strcpy(fname, found_file);
	get_next_file_num++;

	return 1;
}

int sfs_getfilesize(const char* path) {
	int inode_num = -1;

	// search directory for file and get its inode num
	for (int i = 0; i < 100; i++) {
		if (strcmp(directory[i].filename, path) == 0) {
			inode_num = directory[i].inode;
			break;
		}
	}
	// if no directory entry is found, return -1
	if (inode_num == -1) {
		return -1;
	}
	// get file size from file's inode
	return inode_table[inode_num].filesize;
}