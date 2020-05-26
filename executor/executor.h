#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdio.h>                         //for i/o
#include <stdlib.h>                        //for standard utility functions (exit, sleep)
#include <sys/types.h>                     //for sem_t and other standard system types
#include <stdint.h>                        //for standard int types
#include <unistd.h>                        //for sleep
#include <pthread.h>                       //for POSIX threads
#include <signal.h> // Used to handle SIGTERM, SIGINT, SIGKILL
#include "../runtime_util.h"  //for runtime constants (TODO: consider removing relative pathname in include)

// Received a signal to stop the process
// Clean up shared memory
void sigintHandler(int sig_num);



#endif
