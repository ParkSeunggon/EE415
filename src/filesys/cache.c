#include "filesys/cache.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/free-map.h"
#include "devices/block.h"
#include <string.h>
#include <stdio.h>

#define BUFFER_CACHE_ENTRY_NB 64

void *p_buffer_cache;
struct buffer_head buffer_head_array[BUFFER_CACHE_ENTRY_NB];
int clock_hand;


void bc_init(void);
void bc_term(void);
void bc_flush_entry(struct buffer_head *p_flush_entry);
void bc_flush_all_entries(void);

bool bc_read(block_sector_t sector_idx, void *buffer, off_t bytes_read, int chunk_size, int sector_ofs);

bool bc_write(block_sector_t sector_idx, void *buffer, off_t bytes_written, int chunk_size, int sector_ofs);
struct buffer_head* bc_lookup(block_sector_t sector);

struct buffer_head* bc_select_victim(void);

void bc_init(void){
	p_buffer_cache = malloc(BLOCK_SECTOR_SIZE * BUFFER_CACHE_ENTRY_NB);
	if(p_buffer_cache == NULL) return;
	int i;
	clock_hand = 0;
	for(i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
		buffer_head_array[i].is_dirty = false;
		buffer_head_array[i].is_accessed = false;
		buffer_head_array[i].data = p_buffer_cache + BLOCK_SECTOR_SIZE * i;
		buffer_head_array[i].clock = false;
		lock_init(&(buffer_head_array[i].buffer_lock));
	}
	
}

void bc_term(){
	bc_flush_all_entries();
	free(p_buffer_cache);
}

void bc_flush_entry(struct buffer_head *p_flush_entry){
	if(p_flush_entry->is_accessed == false || p_flush_entry->is_dirty == false){
		return;
	}
	p_flush_entry->is_dirty = false;
	block_write(fs_device, p_flush_entry->disk_sector, p_flush_entry->data);
}
void bc_flush_all_entries(void){
	int i;
	for(i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
		bc_flush_entry(&(buffer_head_array[i]));
	}
}

struct buffer_head* bc_lookup(block_sector_t sector){
	int i;
	for(i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
		if(buffer_head_array[i].is_accessed==true && buffer_head_array[i].disk_sector == sector){
			return &(buffer_head_array[i]);
		}
	}
	return NULL;
}

struct buffer_head* bc_select_victim(void){
	int i;
	
	//if entry is not full
	for(i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
		if(buffer_head_array[i].is_accessed == false){
			return &(buffer_head_array[i]);
		}
	}
	
	//entry is full
	while(true){
		//victim selection
		if(buffer_head_array[clock_hand].clock == false){
			if(buffer_head_array[clock_hand].is_dirty == true && buffer_head_array[clock_hand].is_accessed == true){	
				bc_flush_entry(&buffer_head_array[clock_hand]);	
			}
			buffer_head_array[clock_hand].is_accessed = false;
			//buffer_head_array[clock_hand].disk_sector = -1;
			buffer_head_array[clock_hand].clock = false;
			return &(buffer_head_array[clock_hand]);
		}
		else{
			buffer_head_array[clock_hand].clock = false;
		}
		clock_hand++;
		//circular idx
		if(clock_hand == BUFFER_CACHE_ENTRY_NB) clock_hand = 0;	
	}
}





bool bc_read(block_sector_t sector_idx, void *buffer, off_t bytes_read, int chunk_size, int sector_ofs){

	struct buffer_head *buf_head;
	buf_head = bc_lookup(sector_idx);
	//not found
	if(buf_head == NULL){
		buf_head = bc_select_victim();
		bc_flush_entry(buf_head);
		//updates victim to given data
		lock_acquire(&(buf_head->buffer_lock));
		buf_head->disk_sector = sector_idx;
		buf_head->is_accessed = true;
		buf_head->is_dirty = false;
		block_read(fs_device, sector_idx, buf_head->data);
		lock_release(&(buf_head->buffer_lock));
	}
	buf_head -> clock = true;
	memcpy(buffer + bytes_read, buf_head->data + sector_ofs , chunk_size);
	return true;
}

bool bc_write(block_sector_t sector_idx, void *buffer, off_t bytes_written, int chunk_size, int sector_ofs){

	struct buffer_head *buf_head;

	buf_head = bc_lookup(sector_idx);
	if(buf_head == NULL){
		buf_head = bc_select_victim();
		bc_flush_entry(buf_head);
		lock_acquire(&(buf_head->buffer_lock));
		buf_head->disk_sector = sector_idx;
		buf_head->is_accessed = true;
		block_read(fs_device, sector_idx, buf_head->data);
		lock_release(&(buf_head->buffer_lock));
	}
	memcpy(buf_head->data + sector_ofs, buffer + bytes_written, chunk_size);
	lock_acquire(&(buf_head->buffer_lock));	
	buf_head -> clock = true;
	buf_head -> is_dirty = true;
	lock_release(&(buf_head->buffer_lock));
	return true;
}



