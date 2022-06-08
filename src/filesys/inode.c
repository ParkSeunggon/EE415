#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include <stdio.h>
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44


#define DIRECT_BLOCK_ENTRIES 123
#define INDIRECT_BLOCK_ENTRIES 128

enum direct_t{
	NORMAL_DIRECT,
	INDIRECT,
	DOUBLE_INDIRECT,
	OUT_LIMIT
};

struct sector_location{
	char directness;
	off_t index1;
	off_t index2;
};

struct inode_indirect_block{
	block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t is_dir;
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    block_sector_t indirect_block_sec;
    block_sector_t double_indirect_block_sec;
  };

struct inode
{
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    bool removed;                       /* True if deleted, false otherwise. */
    int open_cnt;                       /* Number of openers. */
    struct lock extend_lock;
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */

};
//pj 4
static bool get_disk_inode(const struct inode *inode, struct inode_disk *inode_disk);
static void locate_byte(off_t pos, struct sector_location *sec_loc);
static bool register_sector(struct inode_disk *inode_disk, block_sector_t new_sector, struct sector_location sec_loc);
static block_sector_t byte_to_sector(const struct inode_disk *inode_disk, off_t pos);
bool inode_update_file_length(struct inode_disk *inode_disk, off_t start_pos, off_t end_pos);
static void free_inode_sectors(struct inode_disk *inode_disk);
bool inode_is_dir(const struct inode *inode);



static bool get_disk_inode(const struct inode *inode, struct inode_disk *inode_disk){
	//read sector from bc and save into inode_disk
	return bc_read(inode->sector, (void *)inode_disk ,0, BLOCK_SECTOR_SIZE, 0);
	//return true;
}

static void locate_byte(off_t pos, struct sector_location *sec_loc){
	//set sector location by offset
	off_t pos_sector = pos / BLOCK_SECTOR_SIZE;


	if(pos_sector < DIRECT_BLOCK_ENTRIES){
		sec_loc->directness = NORMAL_DIRECT;
		sec_loc->index1 = pos_sector;
	}
	else if(pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES) ){
		//if(pos_sector == 123) //printf("pos_sector : %d\n",pos_sector);
		sec_loc->directness = INDIRECT;
		//index range 0~127
		sec_loc->index1 = pos_sector - DIRECT_BLOCK_ENTRIES;
	}
	else if(pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES *(INDIRECT_BLOCK_ENTRIES + 1))){
		sec_loc->directness = DOUBLE_INDIRECT;
		//index range 0~127
		sec_loc->index1 = (pos_sector-DIRECT_BLOCK_ENTRIES-INDIRECT_BLOCK_ENTRIES)/INDIRECT_BLOCK_ENTRIES;
		sec_loc->index2 = (pos_sector-DIRECT_BLOCK_ENTRIES-INDIRECT_BLOCK_ENTRIES)%INDIRECT_BLOCK_ENTRIES;
	
	}

	else {
		sec_loc->directness = OUT_LIMIT;
	}
}
static bool register_sector(struct inode_disk *inode_disk, block_sector_t new_sector, struct sector_location sec_loc){
	//update new_sector into inode_disk

	block_sector_t *new_block;
	switch(sec_loc.directness){
		case NORMAL_DIRECT:
			inode_disk->direct_map_table[sec_loc.index1] = new_sector;
			break;
		case INDIRECT:
			if(sec_loc.index1 == 0){
				//first location
				block_sector_t sector_idx;
				free_map_allocate(1, &sector_idx);
				inode_disk->indirect_block_sec = sector_idx;
			}
			//printf("register sector : %d\n",inode_disk->indirect_block_sec);
			new_block = (block_sector_t *) malloc(BLOCK_SECTOR_SIZE);
			if(new_block==NULL) return false;
			new_block[sec_loc.index1] = new_sector;
			off_t ind_ofs = 4 * sec_loc.index1;
			bc_write(inode_disk->indirect_block_sec,new_block,ind_ofs,4,ind_ofs);	
			free(new_block);
			break;
		case DOUBLE_INDIRECT:
			new_block = (block_sector_t *)malloc(BLOCK_SECTOR_SIZE);
			if(new_block == NULL) return false;
			
			//first & second block
			block_sector_t *f_ind_block = (block_sector_t *)malloc(BLOCK_SECTOR_SIZE);
			if(f_ind_block == NULL) {
				free(new_block);
				return false;
			}
			block_sector_t *d_ind_block = (block_sector_t *)malloc(BLOCK_SECTOR_SIZE);
			if(d_ind_block == NULL){ 
				free(new_block);
				return false;
			}
			if(sec_loc.index1 == 0 && sec_loc.index2 == 0){
				//very first double indir 
				block_sector_t sector_idx;
				if(free_map_allocate(1, &sector_idx)) inode_disk->double_indirect_block_sec = sector_idx;
				else {
					free(new_block);
					return false;
				}
			}
			if(sec_loc.index2==0){
				//first connected indir block
				block_sector_t sector_idx;
				if(free_map_allocate(1, &sector_idx)){
					new_block[sec_loc.index1] = sector_idx;
					off_t ofs = 4 * sec_loc.index1;
					bc_write(inode_disk->double_indirect_block_sec,(void *)new_block,ofs,4,ofs);
				}
				else {
					free(new_block);
					return false;
			
				}
			}

			//read data from double_indirect_block to first ind block, get blocksector
			bc_read(inode_disk->double_indirect_block_sec,f_ind_block,0,BLOCK_SECTOR_SIZE,0);
			block_sector_t first_block_idx =  f_ind_block[sec_loc.index1];
		
			//allocate new sector in the block and write on the first ind block	
			d_ind_block[sec_loc.index2] = new_sector;
			off_t ofs = 4 * sec_loc.index2;
			bc_write(first_block_idx, d_ind_block, ofs, 4, ofs);			
			free(f_ind_block);
			free(d_ind_block);
			free(new_block);
			break;
		default :
			return false;
	}

	return true;
}
static block_sector_t byte_to_sector(const struct inode_disk *inode_disk, off_t pos){
	// find block sector number by using inode_disk and offset
	block_sector_t result_sec;

	block_sector_t *ind_block;
	//struct inode_indirect_block *second_ind_block;

	if(pos < inode_disk->length){
		struct sector_location sec_loc;
		locate_byte(pos,&sec_loc);
		switch(sec_loc.directness){
			case NORMAL_DIRECT:
				result_sec = inode_disk->direct_map_table[sec_loc.index1];
				break;
			case INDIRECT:
				ind_block = (block_sector_t *)malloc(BLOCK_SECTOR_SIZE);
				if(ind_block){
					//printf("byte to sector : %d\n", inode_disk->indirect_block_sec);
					bc_read(inode_disk->indirect_block_sec,ind_block,0,BLOCK_SECTOR_SIZE,0);
					result_sec = ind_block[sec_loc.index1];
				}
				else result_sec = 0;
				free(ind_block);
				break;
			case DOUBLE_INDIRECT:	
				ind_block = malloc(BLOCK_SECTOR_SIZE);
				block_sector_t *d_ind_block = malloc(BLOCK_SECTOR_SIZE);
				if(ind_block && d_ind_block){
					//1. read double_ind_sec and write it on to block
					//2. read the second block, find result sec using index
					bc_read(inode_disk->double_indirect_block_sec,(void *)d_ind_block,0,BLOCK_SECTOR_SIZE,0);
					bc_read(d_ind_block[sec_loc.index1],(void *)ind_block,0,BLOCK_SECTOR_SIZE,0);
					result_sec = ind_block[sec_loc.index2];

				}
				else result_sec = 0;
				free(ind_block);
				free(d_ind_block);
				return 0;
				break;
			default:
				return 0;
				break;
		
		}
	
	}
	else{
		result_sec = 0;
	}
	return result_sec;

}

bool inode_update_file_length(struct inode_disk *inode_disk, off_t start_pos, off_t end_pos){
	
	off_t size = end_pos - start_pos;
	off_t offset = start_pos;
	void *zeroes = malloc(BLOCK_SECTOR_SIZE);
	off_t chunk_size;

	memset(zeroes, 0, BLOCK_SECTOR_SIZE);
	while(size > 0){
		//printf("size, offset : %d, %d\n",size, offset);	
		int sector_ofs = offset % BLOCK_SECTOR_SIZE;
		if(size > BLOCK_SECTOR_SIZE - sector_ofs){
			chunk_size = BLOCK_SECTOR_SIZE - sector_ofs;
		}
		else chunk_size = size;
		if(sector_ofs > 0){
			//printf("already allocated\n");
			//already allocated
		}
		else{
			block_sector_t sector_idx; //for newly allcate freemap
			struct sector_location sec_loc;
			if(free_map_allocate(1, &sector_idx)){
				locate_byte(offset, &sec_loc);
				register_sector(inode_disk, sector_idx, sec_loc);
			}
			else{
				//printf("reach here??\n");
				free(zeroes);
				return false;
			}
			bc_write(sector_idx, zeroes, 0, BLOCK_SECTOR_SIZE, 0);		
		}
		size -= chunk_size;
		offset += chunk_size;
	}

	free(zeroes);
	return true;
}

static void free_inode_sectors(struct inode_disk *inode_disk){
	int i,j;
	if(inode_disk->double_indirect_block_sec > 0){
		struct inode_indirect_block *ind_block_1 = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);
		struct inode_indirect_block *ind_block_2 = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);
		i=0;
		bc_read(inode_disk->double_indirect_block_sec,ind_block_1,0,BLOCK_SECTOR_SIZE,0);
		while(ind_block_1->map_table[i]>0){
			bc_read(ind_block_1->map_table[i],ind_block_2,0,BLOCK_SECTOR_SIZE,0);
			j=0;
			while(ind_block_2->map_table[j]>0){
				free_map_release(ind_block_2->map_table[j],1);	
				j++;
			}
			free_map_release(ind_block_1->map_table[i],1);
			i++;
		}
		free_map_release(inode_disk->double_indirect_block_sec,1);
		free(ind_block_1);
		free(ind_block_2);	
	}
	
	if(inode_disk->indirect_block_sec > 0){
		struct inode_indirect_block *ind_block = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);
		bc_read(inode_disk->indirect_block_sec,ind_block,0,BLOCK_SECTOR_SIZE,0);
		//printf("freeing\n");
		i=0;
		while(ind_block->map_table[i]>0){
			free_map_release(ind_block->map_table[i],1);
			i++;
		}
		free_map_release(inode_disk->indirect_block_sec,1);
		free(ind_block);
	}

	i=0;
	while(inode_disk->direct_map_table[i] > 0){
		free_map_release(inode_disk->direct_map_table[i],1);
		i++;
	}

}

bool inode_is_dir(const struct inode *inode){
	bool result;
	struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
	get_disk_inode(inode, disk_inode);
	result = disk_inode->is_dir;
	free(disk_inode);
	return result;

}


/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}


/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. 
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}*/

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      //size_t sectors = bytes_to_sectors (length);
      disk_inode->magic = INODE_MAGIC;
      disk_inode->length = length;
      disk_inode->is_dir = is_dir;
      //printf("%d\n", length);
      if(length > 0){
      	if(inode_update_file_length(disk_inode, 0, length)==false){
		free(disk_inode);
		return false;
	}
      }
     
      bc_write(sector,disk_inode,0,BLOCK_SECTOR_SIZE,0);
      free(disk_inode);
      success = true;
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->extend_lock);
  //block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
		struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
		get_disk_inode(inode,disk_inode);
		free_inode_sectors(disk_inode);
		free_map_release(inode->sector,1);
		free(disk_inode);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;


  struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
  get_disk_inode(inode,disk_inode);
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
     
    
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      bc_read(sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  free(disk_inode);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,off_t offset) 
{

  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  //uint8_t *bounce = NULL;


  struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
  if(disk_inode==NULL) return 0;

  if (inode->deny_write_cnt) 
    return 0;
  if(!get_disk_inode(inode, disk_inode)) return 0;

  lock_acquire(&inode->extend_lock);
  int old_length = disk_inode->length;
  int write_end = offset + size - 1;
  if(write_end  > old_length - 1){
	  inode_update_file_length(disk_inode,old_length,write_end); 
	  disk_inode->length += write_end - old_length + 1;
  }

  lock_release(&inode->extend_lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
	    block_sector_t sector_idx = byte_to_sector (disk_inode, offset);

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;


      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      bc_write(sector_idx, (void *)buffer, bytes_written, chunk_size, sector_ofs);
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  bc_write(inode->sector,disk_inode,0, BLOCK_SECTOR_SIZE, 0);

  free(disk_inode);
  
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/*Returns the length, in bytes, of INODE's data.*/
off_t
inode_length (const struct inode *inode)
{
 	struct inode_disk disk_inode;
 	get_disk_inode(inode,&disk_inode);
	return disk_inode.length; 
}
