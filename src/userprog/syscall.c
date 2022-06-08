#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <devices/shutdown.h>
#include <filesys/filesys.h>
#include <userprog/process.h>
#include <filesys/file.h>
#include <devices/input.h>
#include <string.h>
#include <threads/vaddr.h>
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"

static void syscall_handler (struct intr_frame *);

//project 2
void check_address(void *addr);
void get_argument(void *esp, int *arg, int count);
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
tid_t exec(const char *cmd_line);
int wait(tid_t tid);
int open(const char*file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
void sigaction(int signum, void(*handler)(void));
void sendsig(tid_t pid, int signum);
void sched_yield(void);

//pj 4 system calls:
bool sys_isdir(int fd);
bool sys_chdir(const char *dir);
bool sys_mkdir(const char *dir);
bool sys_readdir(int fd, char *name);
int sys_inumber(int fd);



//project 2
void check_address(void *addr){
	if(addr==NULL||!is_user_vaddr(addr)){
		exit(-1);
	}
	if((unsigned int *)addr > (unsigned int *) 0xc0000000 || (unsigned int *)addr < (unsigned int *) 0x8048000 ){
		exit(-1);
	}
}


//put stacked arguments in arg
void get_argument(void *esp, int *arg, int count){
	int i;
	void *sp = esp + 4;
	for(i = 0; i<count; i++){
		check_address(sp);
		arg[i] = *(int *)sp;
		sp = sp + 4;
	}
}

void halt(void){
	shutdown_power_off();	
}

void exit(int status){
	struct thread *t = thread_current();
	t->exit_status = status;
	printf("%s: exit(%d)\n", t->name, status);
	thread_exit();
}

int wait(tid_t tid){
	return process_wait(tid); //exit status of tid
}

bool create(const char *file, unsigned initial_size){
	if(file!=NULL){
		return filesys_create(file, initial_size);} 
	else return false;
}

bool remove(const char *file){
	return filesys_remove(file);
}


tid_t exec(const char *cmd_line){
	tid_t pid = process_execute(cmd_line); //pid : pid of child process 
	struct thread *child = get_child_process((int)pid);
	sema_down(&(child->load_semaphore)); //wait until load by sema_down
	if(child->load_process){
		return pid;
	}
	else return -1;
}

int open(const char *file){
	if(file!=NULL){
		struct file *f = filesys_open(file);
		if(f==NULL) {
			return -1;
		}
		if(strcmp(thread_name(),file)==0) file_deny_write(f);
		int next_fd = process_add_file(f);
		//printf("next fd : %d\n",next_fd);
		return next_fd;}
	return -1;
}

int filesize(int fd){
	struct file *f = process_get_file(fd);
	if(f==NULL) return -1;
	return file_length(f);
}

int read(int fd, void *buffer, unsigned size){
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(fd==0){
		char input_byte;
		input_byte = input_getc();
		unsigned int i=0;
		for(i=0; i<size; i++){
			((char *)buffer)[i] = input_byte;
		}
		lock_release(&filesys_lock);	
		return strlen((char *)buffer);
	}
	else{
		unsigned int f_size =file_read(f,buffer,size);
		lock_release(&filesys_lock);
		return f_size>size? size : f_size;	
	}	

}

int write(int fd, void *buffer, unsigned size){
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);


	if(fd ==1){
		putbuf((const char *)buffer,size);
		lock_release(&filesys_lock);
		return strlen(buffer);
	}
	else{
		bool is_dir;
		is_dir = inode_is_dir(file_get_inode(f));
		if(is_dir){
			lock_release(&filesys_lock);
			return -1;
		}
		int bw = file_write(f,buffer,size);
		lock_release(&filesys_lock);
		return bw;
	}
}

void seek(int fd, unsigned position){
	struct file *f = process_get_file(fd);
	file_seek(f, position);

}
unsigned tell(int fd){
	struct file *f = process_get_file(fd);
	return file_tell(f);

}
void close(int fd){
	process_close_file(fd);
}


void sigaction(int signum, void(*handler)(void)){
	struct thread *t = thread_current();
	t->handler[signum] = handler;
//	printf("handler address is %p\n",handler);
}


void sendsig(tid_t pid, int signum){
	struct thread *t;
	t = get_child_process(pid);
	void (*address)(void) =  t->handler[signum];
	if(address==NULL) return;	
	printf("Signum: %d, Action: %p\n",signum,address);
}

void sched_yield(void){
	thread_yield();
}

bool sys_isdir(int fd){
	struct file *file;
	file = process_get_file(fd);
	if(file==NULL) return false;
	return inode_is_dir(file_get_inode(file));
}

bool sys_chdir(const char *dir){
	struct file *new_dir = filesys_open(dir);
	if(new_dir == NULL) return false;
	//change cur thread's directory
	dir_close(thread_current()->dir);
	thread_current()->dir = dir_open(file_get_inode(new_dir));
	return true;
}
bool sys_mkdir(const char *dir){
	return filesys_create_dir(dir);
}
bool sys_readdir(int fd, char *name){
	bool is_dir;
	struct file *file;
	file = process_get_file(fd);
	is_dir = inode_is_dir(file_get_inode(file));
	
	struct dir *dir;
	int i = 0;
	if(is_dir){
		char subdir[100];
		dir = dir_open(file_get_inode(file));
		while(dir_readdir(dir, subdir)){
			if(strcmp(subdir, ".")==0||strcmp(subdir,"..")==0)
				continue;
			strlcpy(&name[i],subdir,strlen(subdir)+1);
			i = strlen(subdir)+1;
		}
	
	}
	else return false;

	if(strlen(name)==0) return false;
	//printf("readdir name is :%s\n", name);
	return true;
}

int sys_inumber(int fd){
	struct file *file = process_get_file(fd);
	if(file == NULL) return -1;
	return inode_get_inumber(file_get_inode(file));
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock); 
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int arg[3];
  uint32_t *sp = f->esp;
  check_address((void *)sp);
  int number = *sp;



  switch(number){
  	case SYS_HALT:
		halt();
		return;

	case SYS_EXIT:
	       	get_argument(sp,arg,1);
		exit(arg[0]);
		return;	
	case SYS_EXEC:
		get_argument(sp,arg,1);
		check_address((void *)arg[0]);
		f->eax = exec((const char *)arg[0]);
		return;
	case SYS_WAIT:
		get_argument(sp,arg,1);
		f->eax = wait((tid_t)arg[0]);
		return;
	case SYS_CREATE:
		get_argument(sp,arg,2);
		check_address((void *)arg[0]);
		if((void *)arg[0] == NULL) exit(-1);
		f->eax = create((const char *)arg[0],(unsigned)arg[1]);	
		return;
	case SYS_REMOVE:
		get_argument(sp,arg,1);
		check_address((void *)arg[0]);
		f->eax = remove((const char *)arg[0]); 
		return;
	case SYS_READ:
		get_argument(sp,arg,3);
		check_address((void *)arg[1]);
		f->eax = read((int)arg[0], (void *)arg[1],(unsigned)arg[2]);
		return;
	case SYS_WRITE:
		get_argument(sp,arg,3);	
		check_address((void *)arg[1]);
		f->eax = write((int)arg[0], (void *)arg[1],(unsigned)arg[2]);
		return;
	case SYS_OPEN:
		get_argument(sp,arg,1);
		check_address((void *)arg[0]);
		f->eax = open((const char *)arg[0]);
		return;
	case SYS_CLOSE:
		get_argument(sp,arg,1);
		close((int)arg[0]);
		return;
	case SYS_FILESIZE:
		get_argument(sp,arg,1);
		f->eax = filesize((int)arg[0]);
		return;
	case SYS_TELL:
		get_argument(sp,arg,1);
		f->eax = tell((int)arg[0]);
		return;
	
	case SYS_SEEK:
		get_argument(sp,arg,2);
		seek((int)arg[0], (unsigned)arg[1]);
		return;
	case SYS_SIGACTION:
		get_argument(sp,arg,2);
		check_address((void *)arg[1]);
		sigaction((int)arg[0], (void *)arg[1] );
		return;
	case SYS_SENDSIG:
		get_argument(sp,arg,2);
		sendsig((tid_t)arg[0],(int)arg[1]);
		return;
	case SYS_YIELD:
		sched_yield();
		return;

	//pj 4
 	case SYS_ISDIR:
		get_argument(sp, arg, 1);
		f->eax = sys_isdir((int)arg[0]);
		return;
	case SYS_CHDIR:
		get_argument(sp, arg, 1);
		check_address((void *)arg[0]);
		f->eax = sys_chdir((char *)arg[0]);
		return;
	case SYS_MKDIR:
		get_argument(sp, arg, 1);
		check_address((void *)arg[0]);
		f->eax = sys_mkdir((char *)arg[0]);
		return;
	case SYS_READDIR:
		get_argument(sp,arg,2);
		check_address((void *)arg[1]);
		f->eax = sys_readdir((int)arg[0],(char *)arg[1]);
		return;
	case SYS_INUMBER:
		get_argument(sp,arg,1);
		f->eax = sys_inumber((int) arg[0]);
		return;
  }
  
  thread_exit ();
}
