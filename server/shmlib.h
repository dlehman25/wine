#ifndef SERVER_SHMLIB_H
#define SERVER_SHMLIB_H

shm_ptr_t shm_malloc(size_t sz);
void  shm_free(shm_ptr_t ptr);

int  shm_term(void);
int  shm_attach(void);

#endif
