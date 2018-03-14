#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
