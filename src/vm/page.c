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
#define MAX_STACK_SIZE (8 * 1024 * 1024)
static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
bool is_stack_access(const void *addr, void *esp);
void spt_init(struct thread *t) {
  hash_init(&t->spt, page_hash, page_less, NULL);
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
  return hash_insert(&thread_current()->spt, &p->elem) == NULL;
}

struct page *spt_find(void *upage) {
  struct page temp;
  temp.upage = pg_round_down(upage);
  struct hash_elem *e = hash_find(&thread_current()->spt, &temp.elem);
  return e != NULL ? hash_entry(e, struct page, elem) : NULL;
}

bool spt_remove(void *upage) {
  struct page temp;
  temp.upage = pg_round_down(upage);
  struct hash_elem *e = hash_delete(&thread_current()->spt, &temp.elem);
  return e != NULL;
}


bool spt_load(struct page *p) {
    ASSERT(p != NULL);

    // 1. 물리 프레임 할당
    void *kpage = frame_alloc(PAL_USER, p->upage);
    if (kpage == NULL) {
        printf("spt_load: frame_alloc FAILED for upage %p!\n", p->upage);
        return false;
    }

    if (p->loc == PAGE_FILE) {
        lock_acquire(&filesys_lock);
        if (file_read_at(p->file, kpage, p->read_bytes, p->offset) != (off_t)p->read_bytes) {
            lock_release(&filesys_lock); // 실패 시에도 반드시 lock을 풀어줘야 합니다!
            frame_free(kpage);
            return false;
        }
        lock_release(&filesys_lock); // 성공 시에도 lock을 풀어줍니다.
        
        memset((uint8_t *)kpage + p->read_bytes, 0, p->zero_bytes);}  
    else if (p->loc == PAGE_SWAP) {
        // swap에서 복원
        swap_in(p->swap_index, kpage);
        p->loc = PAGE_IN_MEMORY; 
    } else if (p->loc == PAGE_ZERO) {
        // zero page
        memset(kpage, 0, PGSIZE);
    }

    if (pagedir_get_page(p->owner->pagedir, p->upage) != NULL) {
        frame_free(kpage);
        return true;    
    }

    // 4. 이제 안전하게 페이지를 매핑합니다.
    if (!install_page(p->upage, kpage, p->writable)) {
        frame_free(kpage);
        return false;
    }


    return true;
}
bool is_stack_access(const void *addr, void *esp) {
    // 1. 접근 주소는 유저 영역에 있어야 합니다.
    if (!is_user_vaddr(addr)) {
        return false;
    }

    // 2. 접근 주소는 현재 스택 포인터(esp)보다 32바이트(PUSHA 명령어 크기) 이상 멀리 떨어져 있으면 안 됩니다.
    // 3. 또한 스택의 최대 크기(8MB) 제한을 넘어서도 안 됩니다.
    bool is_close_to_esp = (addr >= esp - 32);
    bool is_within_limit = (PHYS_BASE - pg_round_down(addr) <= MAX_STACK_SIZE);

    return is_close_to_esp && is_within_limit;
}

struct page *spt_find_in_thread(struct thread *t, void *upage) {
    struct page temp;
    temp.upage = pg_round_down(upage);
    struct hash_elem *e = hash_find(&t->spt, &temp.elem);
    return e != NULL ? hash_entry(e, struct page, elem) : NULL;
}

bool stack_growth(void *upage) {
   
    void *kpage = frame_alloc(PAL_USER | PAL_ZERO, upage);
    if (kpage == NULL) {
       
        return false;
    }
   

    bool success = install_page(upage, kpage, true);
    if (!success) {
      
        frame_free(kpage);
        return false;
    }
    struct page *p = malloc(sizeof(struct page));
    if (p == NULL) {
        frame_free(kpage);
        return false;
    }

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
    // 1. Supplemental Page Table에서 페이지 정보를 찾습니다.
    struct page *p = spt_find(pg_round_down(upage));
    if (p == NULL) {
        return false; // SPT에 없는 페이지는 로드할 수 없습니다.
    }
    
    return spt_load(p);
}