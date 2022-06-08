#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"
/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

struct dir* parse_path(char *path_name, char *file_name);
bool filesys_create_dir(const char *name);
/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");
  bc_init();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
  thread_current()->dir = dir_open_root();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  bc_term();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
	char *cp_name = malloc(strlen(name)+1);
	if(cp_name == NULL) return false;
	char *file_name = malloc(strlen(name)+1);
	if(file_name == NULL) {
		free(cp_name);
		return false;}
	strlcpy(cp_name, name, strlen(name)+1);

  block_sector_t inode_sector = 0;
  struct dir *dir = parse_path(cp_name, file_name); //by this we get directory file name

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size,false)
                  && dir_add (dir, file_name, inode_sector));


  if (!success && inode_sector != 0){
	  free_map_release (inode_sector, 1);
  }

  dir_close (dir);
  free(file_name);
  free(cp_name);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
	//name : path name, file_name : file_name
	if(strcmp(name, ".")==0 || strcmp(name, "..")==0) return false;	
	char *cp_name = malloc(strlen(name)+1);
	if(cp_name == NULL) return false;
	char *file_name = malloc(strlen(name)+1);
	if(file_name == NULL) {
		free(cp_name);
		return false;
	}

	strlcpy(cp_name, name, strlen(name)+1);


  struct dir *dir = parse_path(cp_name, file_name); 
  struct inode *inode = NULL;
  if (dir != NULL)
    dir_lookup (dir, file_name, &inode);

  dir_close (dir);
  free(cp_name);
  free(file_name);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
	bool success;
	//cannot remove root
	if(strcmp(name,"/")==0) return false;
	//name : path_name, file_name : file_name	
	char *file_name = malloc(strlen(name)+1);
	if(file_name == NULL) return false;
	char *cp_name = malloc(strlen(name)+1);
	if(cp_name == NULL) {
		free(file_name);
		return false;}
	strlcpy(cp_name, name, strlen(name)+1);

	struct dir *dir;
	
	dir = parse_path(cp_name,file_name);

  struct inode *inode;
  dir_lookup(dir, file_name, &inode);
  
 
  struct dir *new_dir = NULL;
  //when directory -> check inside and delete
  if(inode_is_dir(inode)==true){
	new_dir = dir_open(inode);
	char subdir[100];
 	if(dir_readdir(new_dir,subdir)==true) {
		//file exists
		success = false;
	}
	else success = dir != NULL && dir_remove(dir, file_name); 
  }
  
  //when file -> dlete
  else{
  	success = dir != NULL && dir_remove (dir, file_name);
  }


  dir_close (dir); 
  if(new_dir!=NULL) dir_close(new_dir);
  

  free(cp_name);
  free(file_name);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
	struct dir *root = dir_open_root();
	printf ("Formatting file system...");
  	free_map_create ();
	  if (!dir_create (ROOT_DIR_SECTOR, 16))
    		PANIC ("root directory creation failed");
	  //adding "." and ".." to root directory
	  dir_add(root, ".", ROOT_DIR_SECTOR);
	  dir_add(root, "..", ROOT_DIR_SECTOR);
	  dir_close(root);
	  free_map_close ();
	  printf ("done.\n");
}

struct dir* parse_path(char *path_name, char *file_name){
	//parse path and save file name and returns according directory
	struct dir *dir;
	if(path_name ==NULL || file_name == NULL)
		return NULL;
	if(strlen(path_name)==0)
		return NULL;

	//absolute path/ relative path
	if(path_name[0]=='/'){
		dir = dir_open_root();
	}
	else {
		dir = dir_reopen(thread_current()->dir);
	}


	char *token, *nextToken, *savePtr;
	token = strtok_r(path_name, "/", &savePtr);	
	nextToken = strtok_r(NULL, "/",&savePtr);
	//root directory
	if(token == NULL){
		strlcpy(file_name, ".", strlen(file_name)+1);
		return dir;
	}
	while(token!=NULL && nextToken !=NULL){
		struct inode *inode = NULL;
		if(dir_lookup(dir, token, &inode)==false) // no search dir with name token
		{
			dir_close(dir);
			return NULL;
		}
		if(inode_is_dir(inode)==false){
			dir_close(dir);
			return NULL;
		}
		dir_close(dir);
		dir = dir_open(inode);
		//proceed
		token = nextToken;
		nextToken = strtok_r(NULL,"/",&savePtr);
	}
	strlcpy(file_name,token,strlen(token)+1);
	//printf("path and file names are: %s, %s\n", path_name, file_name);
	return dir;

}

bool filesys_create_dir(const char *name){
	if(name == NULL || strlen(name)==0) return false;

	char *file_name = malloc(strlen(name)+1);
	if(file_name==NULL) return false;
	char *cp_name = malloc(strlen(name)+1);
	if(cp_name==NULL){ 
		free(file_name);
		return false;
	}

	strlcpy(cp_name, name, strlen(name)+1);
	
	struct dir *dir = parse_path(cp_name, file_name);

	struct inode *inode;

	//Already exists
	if(dir_lookup(dir, file_name,&inode)){
		free(file_name);
		free(cp_name);
		return false;
	}
	
	block_sector_t inode_sector = 0;	
	block_sector_t inum = inode_get_inumber(dir_get_inode(dir)); 
	bool success = (dir != NULL
			  && free_map_allocate (1, &inode_sector)
        	          && dir_create(inode_sector,16)
        	          //&& dir_create(inode_sector,inum)
                	  && dir_add (dir, file_name, inode_sector));

	if(!success && inode_sector!=0){
		// fail but free map is allocated
		free_map_release(inode_sector,1);
	}
	if(success == true){
		struct dir *new_dir = dir_open(inode_open(inode_sector));
		dir_add(new_dir, "." , inode_sector); // cur direc
		dir_add(new_dir, "..", inum); //previous direc
		dir_close(new_dir);
	}
	dir_close(dir);
	free(file_name);
	free(cp_name);
	return success;
}


