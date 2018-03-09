#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "process.h"

static void syscall_handler (struct intr_frame *);
void *check_addr(const void*);
struct proc_file *file_search(struct list *files, int fd);

struct proc_file {
   struct file* ptr;
   int fd;
   struct list_elem elem;
};

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int *p = f->esp;

  int system_call = *p;

  switch(system_call){
     case SYS_HALT:
        shutdown_power_off();
	break;

     case SYS_EXIT:
	thread_current()->parent->wasExecuted = true;
	thread_current()->exit_error = *(p+1);
	thread_exit();
	break;

     //we will need to check for a valid address here
     case SYS_EXEC:
	check_addr(*(p+1));
	f->eax = process_execute(*(p+1));
	break;

     case SYS_WAIT:
	break;

     //We will need to check for a valid address here
     case SYS_CREATE:
        check_addr(*(p+4));
	f->eax = filesys_create(*(p+4), *(p+5));
	break;

     case SYS_REMOVE:
	check_addr(*(p+1));
	if(filesys_remove(*(p+1)) == NULL){
	  f->eax = false;
	}{
          f->eax = true;
	}
	break;
     
     case SYS_OPEN:
	check_addr(*(p+1));
	struct file *file_pointer = filesys_open(*(p+1));
	if(file_pointer == NULL){
           f->eax = -1;
	}else{
	   struct proc_file *pfile = malloc(sizeof(*pfile));
	   pfile->ptr = file_pointer;
	   pfile->fd = thread_current()->fd_count;
	   thread_current()->fd_count++;
	   list_push_back (&thread_current()->files, &pfile->elem);
	   f->eax = pfile->fd;
	}
	break;

     case SYS_FILESIZE:
        f->eax = file_length (file_search(&thread_current()->files, *(p+1))->ptr);
	break;

     //TODO: Add locks for reading and writing
     case SYS_READ:
	check_addr(*(p+6));//Not sure why we check for the buffer ptr
	if(*(p+5) == 0){ //If the fd == 0 that means we are going to read from user input via keybrd
	   int i;
	   uint8_t *buffer = *(p+6); //Set our buffer to the one on the stack (at location (p+6))

	   //Loop for the number of bytes that we want to read
	   for(int i=0; i < *(p+7); i++){
              buffer[i] = input_getc(); //Set the buffer at that location to the user input
	   }
	   f->eax = *(p+7); //return the number of bytes read (located at (p+7)
	}else{ //else we know that we are going to read from a file
           struct proc_file *file_pointer = file_search (&thread_current()->files, *(p+5));

	   if(file_pointer == NULL){
              f->eax = -1; // b/c the file wasn't found we return with error code -1
	   }else{
              f->eax = file_read_at (file_pointer->ptr, *(p+6), *(p+7), 0); //Look up this method it takes (file pointer, buffer, bytes to read, 0?)
	   }
	}
	break;

     case SYS_WRITE: 
	check_addr(*(p+6));//Not sure why we check for the buffer ptr
	
        if(*(p+5)==1){ //We need to write to console
           putbuf(*(p+6), *(p+7)); //required to call via pintos docs takes buffer and amount to read as params
	   f->eax = *(p+7); // b/c we wrote to console, we know that we wrote all of the required bytes (spec. at p+7)
	}else{
           struct proc_file *file_pointer = file_search (&thread_current()->files, *(p+5));

	   if(file_pointer == NULL){
              f->eax = -1;
	   }else{
              f->eax = file_write_at (file_pointer->ptr, *(p+6), *(p+7), 0); //Just like read but with right...not sure what it does
	   }
	}
	break;

     case SYS_SEEK:
	break;

     case SYS_TELL:
	break;

     case SYS_CLOSE:
	close_single_file(&thread_current()->files, *(p+1));
	break;

     default:
	printf("SYS CALL NOT HANDLED!\n");
  }
  //thread_exit ();
}

void exit(int status){
   thread_exit(); 
}

/*
 * Inspired by Waqee
 * This method is called whenever we need to do something with a file, 
 * it checks to make sure that the file we want is in user V-memeory
 * and make sure that we have a correct pointer to it in memeory
 * @param const void *vaddr - The virtual address of the file in memeory
 * @return ptr - The pointer to the file
 */ 
void *check_addr(const void *vaddr){

   /*
    * If the given vaddr is not a user vaddr, we tell the parent to execute,
    * say that we closed with an error of -1
    * and exit
    */
   if(!is_user_vaddr(vaddr)){ //If the vaddr is not a user vaddr (that means its kernel vaddr...if we try to access this, we will have a page fault!)
      thread_current()->parent->wasExecuted = true;
      thread_current()->exit_error = -1;
      thread_exit();
      return 0;
   }

   //If the given vaddr is a user vaddr, we get the current page pointer
   void *ptr = pagedir_get_page(thread_current()->pagedir, vaddr);

   /*
    * If we have an invalid pointer, we tell the parent to execute,
    * say that we closed with an error of -1
    * and exit
    */ 
   if(!ptr){
      thread_current()->parent->wasExecuted = true;
      thread_current()->exit_error = -1;
      thread_exit();
      return 0;
   }

   //If it is a user vaddr and we have a vaild pointer we return it
   return ptr;
}

struct proc_file *file_search(struct list *files, int fd){
   struct list_elem *e;

   for(e = list_begin (files); e != list_end (files); e = list_next (e)){
      struct proc_file *file = list_entry (e, struct proc_file, elem);
      if(file->fd == fd)
         return file;
   }

   return NULL;
}

void close_single_file(struct list* files, int fd){
   struct list_elem *e;

   for(e = list_begin (files); e != list_end (files); e = list_next (e)){
      struct proc_file *file = list_entry (e, struct proc_file, elem);
      
      //Maybe break here to save time (IFF we can only have one fd open at a time)
      if(file->fd == fd){
         file_close(file->ptr);
	 list_remove(e);
	 break;
      }
   }
}
void close_files(struct list* files){
   struct list_elem *e;

   for(e = list_begin (files); e != list_end (files); e = list_next (e)){
      struct proc_file *file = list_entry (e, struct proc_file, elem);

      file_close(file->ptr);
      list_remove(e);
   }
}
