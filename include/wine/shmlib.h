#ifndef SHMLIB_H
#define SHMLIB_H

typedef unsigned int shm_ptr_t;
/*
typedef struct
{
    unsigned int segidx : 8;
    unsigned int offset : 24;
} shm_ptr_t;
*/

void *shm_ptr_to_void_ptr(shm_ptr_t shmptr);

#define SHM_NULL 0

#endif
