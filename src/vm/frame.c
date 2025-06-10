#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include <list.h>
#include <debug.h>
#include "vm/page.h"
#include "vm/page.h"         
#include "filesys/filesys.h"    
#include "filesys/file.h"       
#include "threads/vaddr.h"     
#include <stdio.h>      /* printf */
#include "vm/page.h" 
/* Frame table (전역) */
static struct list frame_table;
struct lock frame_lock;
static struct list_elem *clock_hand = NULL;
void
frame_table_init(void) {
  list_init(&frame_table);
  lock_init(&frame_lock);
}
static struct frame_entry *choose_victim(void) {
    if (list_empty(&frame_table)) return NULL;
    if (clock_hand == NULL || clock_hand == list_end(&frame_table))
        clock_hand = list_begin(&frame_table);
    while (true) {
        struct frame_entry *f = list_entry(clock_hand, struct frame_entry, elem);
        if (f->pinned) {
            // 고정된 프레임은 건너뛴다.
            clock_hand = list_next(clock_hand);
            if (clock_hand == list_end(&frame_table))
                clock_hand = list_begin(&frame_table);
            continue;
        }        
        if (!pagedir_is_accessed(f->owner->pagedir, f->upage)) {
            struct list_elem *old = clock_hand;
            clock_hand = list_next(clock_hand);
            return list_entry(old, struct frame_entry, elem);
        } else {
            pagedir_set_accessed(f->owner->pagedir, f->upage, false);
            clock_hand = list_next(clock_hand);
            if (clock_hand == list_end(&frame_table))
                clock_hand = list_begin(&frame_table);
        }
    }
}

/* In vm/frame.c */
void *
frame_alloc(enum palloc_flags flags, void *upage) {
    ASSERT((flags & PAL_USER) != 0);
    lock_acquire(&frame_lock);

    void *kpage = palloc_get_page(flags);

    if (kpage == NULL) {
        /* ----- Eviction Logic START ----- */
        struct frame_entry *victim = choose_victim();
        if (victim == NULL) {
             PANIC("All frames are pinned, cannot evict!");
        }

        struct page *victim_page = spt_find_in_thread(victim->owner, victim->upage);
        ASSERT(victim_page != NULL);

        /* --- Victim Info (for debugging) --- */
        bool is_dirty = pagedir_is_dirty(victim->owner->pagedir, victim->upage);

        if (victim_page->file) { 
            /* 페이지가 파일 기반인 경우 (실행 파일 또는 mmap) */

            if (is_dirty) {
                
                lock_acquire(&filesys_lock);
                file_write_at(victim_page->file, victim->kpage, PGSIZE, victim_page->offset);
                lock_release(&filesys_lock);
            }
            victim_page->loc = PAGE_FILE;
        } else { 
            /* 페이지가 익명(anonymous)인 경우 */

            victim_page->loc = PAGE_SWAP;
            victim_page->swap_index = swap_out(victim->kpage);
        }

        
        pagedir_clear_page(victim->owner->pagedir, victim->upage);
        kpage = victim->kpage;
        list_remove(&victim->elem);
        free(victim);
        /* ----- Eviction Logic END ----- */
    }

    struct frame_entry *f = malloc(sizeof(struct frame_entry));
    if (f == NULL) {
        palloc_free_page(kpage);
        lock_release(&frame_lock);
        return NULL;
    }
    
    f->kpage = kpage;
    f->upage = upage;
    f->owner = thread_current();
    f->pinned = false;
    list_push_back(&frame_table, &f->elem);

    lock_release(&frame_lock);
    return kpage;
}

void
frame_free(void *kpage) {
  ASSERT(kpage != NULL);
  lock_acquire(&frame_lock);

  struct list_elem *e;
  for (e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)) {
    struct frame_entry *f = list_entry(e, struct frame_entry, elem);
    if (f->kpage == kpage) {
      list_remove(e);
      free(f);
      break;
    }
  }

  palloc_free_page(kpage);
  lock_release(&frame_lock);
}
static struct frame_entry* find_frame(void *kpage) {
    struct list_elem *e;
    for (e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)) {
        struct frame_entry *f = list_entry(e, struct frame_entry, elem);
        if (f->kpage == kpage) {
            return f;
        }
    }
    return NULL;
}

// 프레임 고정
void frame_pin(void *kpage) {
    lock_acquire(&frame_lock);
    struct frame_entry *f = find_frame(kpage);
    if (f) {
        f->pinned = true;
    }
    lock_release(&frame_lock);
}

// 프레임 고정 해제
void frame_unpin(void *kpage) {
    lock_acquire(&frame_lock);
    struct frame_entry *f = find_frame(kpage);
    if (f) {
        f->pinned = false;
    }
    lock_release(&frame_lock);
  }