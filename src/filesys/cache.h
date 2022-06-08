#ifndef CACHE_H
#define CACHE_H
#include "threads/synch.h"
#include "devices/block.h"
#include "filesys/inode.h"
#include "filesys/filesys.h"
struct buffer_head
{
	bool is_dirty;
	bool is_accessed;
	block_sector_t disk_sector;
	struct lock buffer_lock;
	bool clock;
	void *data;
};


void bc_init(void);
void bc_term(void);
void bc_flush_entry(struct buffer_head *p_flush_entry);
void bc_flush_all_entries(void);
struct buffer_head* bc_lookup(block_sector_t sector);
struct buffer_head* bc_select_victim(void);


bool bc_read(block_sector_t sector_idx, void *buffer, off_t bytes_read, int chunk_size, int sector_ofs);
bool bc_write(block_sector_t sector_idx, void *buffer, off_t bytes_written, int chunk_size, int sector_ofs);

#endif
