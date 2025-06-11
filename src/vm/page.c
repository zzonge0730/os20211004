#include "vm/page.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include <string.h>
#include "userprog/process.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#include <stdio.h>
#include "filesys/filesys.h"
#include "vm/page.h"
#include <stdio.h>

#define MAX_STACK_SIZE (8 * 1024 * 1024)

static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

void
spt_init (struct thread *t) 
{
  hash_init (&t->spt, page_hash, page_less, NULL);
  lock_init (&t->spt_lock); 
  
}

unsigned page_hash(const struct hash_elem *e, void *aux UNUSED) {
  struct page *p = hash_entry(e, struct page, elem);
  return hash_bytes(&p->upage, sizeof p->upage);
}

bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
  struct page *pa = hash_entry(a, struct page, elem);
  struct page *pb = hash_entry(b, struct page, elem);
  return pa->upage < pb->upage;
}

bool spt_insert(struct page *p) {
  struct thread *t = p->owner;
  lock_acquire(&t->spt_lock);
  bool success = hash_insert(&t->spt, &p->elem) == NULL;
  lock_release(&t->spt_lock);
  return success;
}

struct page *spt_find(void *upage) {
  struct thread *t = thread_current();
  lock_acquire(&t->spt_lock);

  struct page temp;
  temp.upage = pg_round_down(upage);
  struct hash_elem *e = hash_find(&t->spt, &temp.elem);
  struct page *result = (e != NULL) ? hash_entry(e, struct page, elem) : NULL;
  
  lock_release(&t->spt_lock);
  return result;
}

bool spt_remove(void *upage) {
  struct thread *t = thread_current();
  lock_acquire(&t->spt_lock);

  struct page temp;
  temp.upage = pg_round_down(upage);
  struct hash_elem *e = hash_delete(&t->spt, &temp.elem);

  lock_release(&t->spt_lock);
  return e != NULL;
}

bool spt_load(struct page *p) {
    ASSERT(p != NULL);
  
    void *kpage = frame_alloc(PAL_USER, p->upage);
    if (kpage == NULL) {
        
        return false;
    }

    if (p->loc == PAGE_FILE) {
        lock_acquire(&filesys_lock);
        if (file_read_at(p->file, kpage, p->read_bytes, p->offset) != (off_t)p->read_bytes) {
            lock_release(&filesys_lock);
            frame_free(kpage);
            return false;
        }
        lock_release(&filesys_lock);
        memset((uint8_t *)kpage + p->read_bytes, 0, p->zero_bytes);
        p->loc = PAGE_IN_MEMORY; 
    } else if (p->loc == PAGE_SWAP) {
        swap_in(p->swap_index, kpage);
        p->loc = PAGE_IN_MEMORY;
    } else if (p->loc == PAGE_ZERO) {
        memset(kpage, 0, PGSIZE);
        p->loc = PAGE_IN_MEMORY;
    }

    if (pagedir_get_page(p->owner->pagedir, p->upage) != NULL) {
        frame_free(kpage);
        return true;
    }

    if (!install_page(p->upage, kpage, p->writable)) {
        if (pagedir_get_page(p->owner->pagedir, p->upage) != NULL) {
            frame_free(kpage);
            return true; // 다른 스레드가 설치함
        }      
        frame_free(kpage);
        return false;
    }
    return true;
}

bool is_stack_access(const void *addr, void *esp) {
    if (!is_user_vaddr(addr)) {
        return false;
    }
    
    uintptr_t addr_val = (uintptr_t)addr;
    uintptr_t esp_val = (uintptr_t)esp;
    uintptr_t stack_bottom = PHYS_BASE - MAX_STACK_SIZE;
    //printf(" KERNEL: [is_stack_access] checking addr=%p against esp=%p\n", addr, esp);
    //printf(" KERNEL: [is_stack_access] lower bound (esp - 32) is %p\n", esp - 32);    
    // PUSHA 명령어를 고려하여 esp 아래 32바이트까지 허용
    bool is_near_esp = (addr_val >= esp_val - 32) || 
                       (addr_val >= esp_val && addr_val < PHYS_BASE);
    bool is_in_stack_range = addr_val >= stack_bottom && addr_val < PHYS_BASE;
    //printf(" KERNEL: [is_near_esp] result: %s\n", is_near_esp ? "TRUE" : "FALSE");
    //printf(" KERNEL: [is_in_stack_range] result: %s\n", is_in_stack_range ? "TRUE" : "FALSE");
    return is_near_esp && is_in_stack_range;
}


struct page *spt_find_in_thread(struct thread *t, void *upage) {
    lock_acquire(&t->spt_lock);
    struct page temp;
    temp.upage = pg_round_down(upage);
    struct hash_elem *e = hash_find(&t->spt, &temp.elem);
    struct page *result = e != NULL ? hash_entry(e, struct page, elem) : NULL;
    lock_release(&t->spt_lock);
    return result;
}

bool
stack_growth(void *upage) {
    void *kpage = frame_alloc(PAL_USER | PAL_ZERO, upage);
    if (kpage == NULL)
        return false;

    if (!install_page(upage, kpage, true)) {
        frame_free(kpage);
        return false;
    }
    
    struct page *p = malloc(sizeof(struct page));
    if (p == NULL) {
        frame_free(kpage);
        return false;
    }

    memset(p, 0, sizeof(struct page));

    p->loc = PAGE_ZERO;
    p->upage = upage;
    p->owner = thread_current();
    p->writable = true;

    if (!spt_insert(p)) {
        free(p);
        frame_free(kpage);
        return false;
    }

    return true;
}

bool spt_load_page(void *upage) {
    struct page *p = spt_find(pg_round_down(upage));
    if (p == NULL) {
        return false;
    }
    return spt_load(p);
}