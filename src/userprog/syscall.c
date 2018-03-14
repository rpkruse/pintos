#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "process.h"
#include "list.h"

static void syscall_handler (struct intr_frame *);
void *isValidAddr(const void*);
struct proc_file *file_search(struct list *files, int fd);

/*inspired by Waqee*/
//A structure to hold process files
struct proc_file {
   struct file* ptr;
   int fd;
   struct list_elem elem;
};

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int *p = f->esp;

  int arg[3]; //We will have at most 3 args
  
  isValidAddr(p);

  int system_call = *p;

  switch(system_call){
     case SYS_HALT: //No args
        shutdown_power_off();
	break;

     case SYS_EXIT: //One arg --> exit status
	get_arg(f, &arg[0], 1);
	exit(arg[0]);
	break;
 
     case SYS_EXEC: //One arg --> file name
	get_arg(f, &arg[0], 1);
	arg[0] = isValidAddr(arg[0]);

	f->eax = process_execute(arg[0]);
	break;

     case SYS_WAIT: //One arg 
	get_arg(f, &arg[0], 1);
	f->eax = process_wait(arg[0]);
	break;
 
     case SYS_CREATE:	
	get_arg(f, &arg[0], 2);
	arg[0] = isValidAddr(arg[0]);

	lock_acquire(&filesys_lock);

	f->eax = filesys_create(arg[0], arg[1]);
	lock_release(&filesys_lock);
	break;

     case SYS_REMOVE:	
	get_arg(f, &arg[0], 1);

	lock_acquire(&filesys_lock);
	if(filesys_remove(arg[0]) == NULL){
	  f->eax = false;
	}{
          f->eax = true;
	}
	lock_release(&filesys_lock);
	break;
     
     case SYS_OPEN:
	get_arg(f, &arg[0], 1);

	arg[0] = isValidAddr(arg[0]);

	lock_acquire(&filesys_lock);

	struct file *file_pointer = filesys_open(arg[0]);
         
	lock_release(&filesys_lock);

	if(file_pointer == NULL){ //If it is a bad file, return exit error -1 
           f->eax = -1;
	}else{ //If it is a good file, open it and add it to our list of files
	   //Create a new file to open
	   struct proc_file *pfile = malloc(sizeof(*pfile));
	   pfile->ptr = file_pointer;
	   //Get a new fd
	   pfile->fd = thread_current()->fd_count;
	   thread_current()->fd_count++; //Increment our fd count

	   list_push_back (&thread_current()->files, &pfile->elem); //add it
	   f->eax = pfile->fd; //return the fd as per request of pintos docs
	}
	break;

     case SYS_FILESIZE:
	get_arg(f, &arg[0], 1);
	lock_acquire(&filesys_lock);
        f->eax = file_length (file_search(&thread_current()->files, *(p+1))->ptr);
	lock_release(&filesys_lock);
	break;

     case SYS_READ:        
	get_arg(f, &arg[0], 3);
	check_valid_buffer((void *) arg[1], (unsigned) arg[2]);
	arg[1] = isValidAddr(arg[1]);

	if(arg[0] == 0){
	   int i;
	   uint8_t *buffer = arg[1]; //Set our buffer to the one on the stack (at location (p+6))

	   //Loop for the number of bytes that we want to read
	   for(int i=0; i < arg[2]; i++){
              buffer[i] = input_getc(); //Set the buffer at that location to the user input
	   }
	   f->eax = arg[2]; //return the number of bytes read (located at (p+7)
	}else{ //else we know that we are going to read from a file
           struct proc_file *file_pointer = file_search (&thread_current()->files, arg[0]);

	   if(file_pointer == NULL){
              f->eax = -1; // b/c the file wasn't found we return with error code -1
	   }else{ 
              lock_acquire(&filesys_lock);
	      f->eax = file_read (file_pointer->ptr, arg[1], arg[2]); 
	      lock_release(&filesys_lock); 
	   }
	}
	break;

     case SYS_WRITE: 	
	get_arg(f, &arg[0], 3);
	check_valid_buffer((void *) arg[1], (unsigned) arg[2]);
	arg[1] = isValidAddr(arg[1]);

        if(arg[0]==1){ //We need to write to console
           putbuf(arg[1], arg[2]); //required to call via pintos docs takes buffer and amount to read as params
	   f->eax = arg[2]; // b/c we wrote to console, we know that we wrote all of the required bytes (spec. at p+7)
	}else{
           struct proc_file *file_pointer = file_search (&thread_current()->files, arg[0]);

	   if(file_pointer == NULL){
              f->eax = -1;
	   }else{ 
	      lock_acquire(&filesys_lock);
              f->eax = file_write(file_pointer->ptr, arg[1], arg[2]);
	      lock_release(&filesys_lock); 
	   }
	}
	break;

     case SYS_SEEK:
	get_arg(f, &arg[0], 2);
	lock_acquire(&filesys_lock);
	file_seek (file_search(&thread_current()->files, arg[0])->ptr, arg[1]);
	lock_release(&filesys_lock);
	break;

     case SYS_TELL:
	get_arg(f, &arg[0], 1);
	lock_acquire(&filesys_lock);
	f->eax = file_tell (file_search(&thread_current()->files, arg[0])->ptr);
	lock_release(&filesys_lock);
	break;

     case SYS_CLOSE:
	get_arg(f, &arg[0], 1);
	lock_acquire(&filesys_lock);
	close_single_file(&thread_current()->files, arg[0]);
	lock_release(&filesys_lock);
	break;

     default:
	printf("Invalid System Call!\n");
	thread_exit();
  }
}

/*
 * Inspired by Waqee
 * This method is called everytime a thread needs to exit.
 * When we exit, we tell each one of our children that we are done and that they can use the lock (f->used)
 * We also set their exit error to ours, if the parent fails, all the children will too.
 * @param int status - The status we are exiting on (error code).
 */
void exit(int status){
   struct list_elem *e;

   /*
    * When we exit, we must tell all of our children (if any) that they can now use the lock
    * we must also set their exit code to ours
    */
   for(e = list_begin (&thread_current()->parent->child_proc); e != list_end (&thread_current()->parent->child_proc); e = list_next (e)){
      struct child *f = list_entry (e, struct child, elem);
      if(f->tid == thread_current()->tid){
         f->used = true; //They can now use the lock
	 f->exit_error = status; //If we fail they fail
      }
   }

   //Set our exit code to status
   thread_current()->exit_error = status; 

   //If our parent is waiting on us, we can release the lock (we are the last to finish)
   if(thread_current()->parent->waitingOn == thread_current()->tid){
      sema_up(&thread_current()->parent->child_lock);
   }

   //We leave
   thread_exit();
}

/*
 * This method is called whenever we need to do something with a file, 
 * it checks to make sure that the file we want is in user V-memeory
 * and make sure that we have a correct pointer to it in memeory
 * If it isn't a user vaddr or is not in memory, we exit with error -1
 * @param const void *vaddr - The virtual address of the file in memeory
 * @return ptr - The pointer to the file
 */ 
void *isValidAddr(const void *vaddr){
   
   //Check to see if the vaddr is a user vaddr and to see if the page is in memeory
   //If it is not one of these we exit with error -1
   if(!is_user_vaddr(vaddr) ||  pagedir_get_page(thread_current()->pagedir, vaddr) == NULL)
      exit(-1);

   //If it is in memeory and a user vaddr we return a pointer to the page
   return pagedir_get_page(thread_current()->pagedir, vaddr); 
}

/*
 * This method is called whenever we are given a buffer and a set size.
 * It makes sure that all of the address in the buffer are valid
 * If they are not valid we exit with error -1 (int isValidAddr)
 * @param void *buffer - The buffer to check
 * @param unsigned size - The size of the buffer
 */
void check_valid_buffer (void *buffer, unsigned size){
   unsigned i;
   char *local_buffer = (char *) buffer;
   for(i=0; i < size; i++){
      isValidAddr((const void *) local_buffer);
      local_buffer++;
   }
}

/*
 * This method is called everytime we want to find a single file.
 * It searches the threads list of files and attempts to find a matching fd
 * If one is found, we return it, else return NULL
 * @param struct list *files - A pointer to a list of files
 * @param int fd - The file decorator (basically file ID)
 * @return struct proc_file - The file we searched for
 */
struct proc_file *file_search(struct list *files, int fd){
   struct list_elem *e;
   struct proc_file *file;

   for(e = list_begin (files); e != list_end (files); e = list_next (e)){
      file = list_entry (e, struct proc_file, elem);
      if(file->fd == fd)
         return file;
   }

   return NULL;
}

/*
 * This method is called everytime we want to close a single file.
 * It searches through our list of files, if a matching one is found,
 * we remove it from the list and call syscall close on that file
 * @param struct list* files - A list of files
 * @param int fd - The file decorator (file ID)
 */
void close_single_file(struct list* files, int fd){
   struct list_elem *e;

   struct proc_file *file;

   for(e = list_begin (files); e != list_end (files); e = list_next (e)){
      file  = list_entry (e, struct proc_file, elem);
      
      //If we find a matching file, close it and remove from our list
      if(file->fd == fd){
         file_close(file->ptr);
	 list_remove(e);
	 break;//There will only be one matching file, so we can break
      }
   } 
}

/*
 * This method is called when we want to close all files that a thread has
 * It works the same as close_single_file, but also frees it from memory
 * @param struct list* files - The list of files the thread currently has
 */
void close_files(struct list* files){
   struct list_elem *e;

  //For each file: close it, remove it from the list, and free it from memory
  while(!list_empty(files)){
      e = list_pop_front(files);

      struct proc_file *file = list_entry (e, struct proc_file, elem);
 
      file_close(file->ptr);
      list_remove(e);
      free(file);
   }
}

/*
 * inspired by ryantimwilson
 * This method is called everytime get a syscall.
 * It pulls the arguments from the stack based on how many args we need
 * and adds them to a list of arguments
 * @param struct intr_frame *f - The pointer to the stack
 * @param int *arg - An array to store the arguments in (global field)
 * @param int n - The number of arguments to search for
 */
void get_arg (struct intr_frame *f, int *arg, int n){
   int i;
   int *ptr;

   int *p = f->esp;
   //For each argument, move the pointer (i+1) up in the stack, (We do i+1 because the first argument will be argv)
   //check if the addr is valid, if so add it to our list of arguments
   for(i = 0; i < n; i++){
      ptr = (int *) f->esp + i + 1;
      isValidAddr((const void *) ptr);
      arg[i] = *ptr;
   }
}
