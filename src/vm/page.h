#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "threads/thread.h"

enum page_location {
    PAGE_FILE,      // lazy load from file
    PAGE_SWAP,      // swapped out
    PAGE_ZERO       // zero page
};

struct page {
    void *upage;                // user virtual address (key)
    struct hash_elem elem;     // hash table element
    struct thread *owner;      // owning thread

    enum page_location loc;    // where the page is
    struct file *file;         // if PAGE_FILE
    size_t offset;             // file offset
    size_t read_bytes;         // bytes to read from file
    size_t zero_bytes;         // bytes to zero
    bool writable;             // is the page writable?
    size_t swap_index;         // if PAGE_SWAP
};

void spt_init(struct thread *t);
bool spt_insert(struct page *p);
struct page *spt_find(void *upage);
bool spt_remove(void *upage);
bool install_page (void *upage, void *kpage, bool writable);
bool is_stack_access(void *addr, void *esp);
bool stack_growth(void *upage);

#endif
