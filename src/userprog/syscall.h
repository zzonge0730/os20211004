#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/thread.h"

mapid_t mmap(int fd, void *addr);
void munmap(mapid_t mapid);
void syscall_init(void);
struct mmap_entry; 
void do_munmap(struct mmap_entry *entry);

#endif /* userprog/syscall.h */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif