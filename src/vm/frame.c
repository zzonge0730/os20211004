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
    //printf(" KERNEL: No free frames, starting eviction...\n"); // Eviction 시작 알림

    struct frame_entry *victim = choose_victim();
    if (victim == NULL) {
         PANIC("All frames are pinned, cannot evict!");
    }

    struct page *victim_page = spt_find_in_thread(victim->owner, victim->upage);
    ASSERT(victim_page != NULL);

    bool is_dirty = pagedir_is_dirty(victim->owner->pagedir, victim->upage);
    
    // --- 희생양(victim)의 정보 상세 출력 ---
    //printf(" KERNEL: Evicting frame. kpage=%p, upage=%p, owner_tid=%d, dirty=%d\n", victim->kpage, victim->upage, victim->owner->tid, is_dirty);
    //printf(" KERNEL: Victim page's initial state: loc=%d\n", victim_page->loc);

    if (victim_page->file) { 
        // ... (파일 기반 페이지 처리)
        victim_page->loc = PAGE_FILE; // 상태 변경
    } else { 
        // ... (익명 페이지 처리)
        victim_page->loc = PAGE_SWAP; // 상태 변경
        victim_page->swap_index = swap_out(victim->kpage);
        //printf(" KERNEL: Swapped out to slot %d.\n", victim_page->swap_index);
    }
    
    // !!! 상태가 정말 변경되었는지 확인하는 로그 !!!
    //printf(" KERNEL: Victim page's new state: loc=%d\n", victim_page->loc);

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
    if (upage == NULL) {
        // upage가 NULL이면 페이지 테이블용 프레임으로 간주하고
        // 영구적으로 pin 처리하여 evict되지 않도록 합니다.
        f->pinned = true;
    } else {
        // 일반 사용자 페이지용 프레임은 evict 대상이 될 수 있습니다.
        f->pinned = false;
    }
    list_push_back(&frame_table, &f->elem);

    lock_release(&frame_lock);
    return kpage;
}

void
frame_free(void *kpage) {
  ASSERT(kpage != NULL);
  //printf("[frame] free frame kpage=%p\n", kpage);
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