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

/* Frame table (전역) */
static struct list frame_table;
static struct lock frame_lock;
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
void *frame_alloc(enum palloc_flags flags, void *upage) {
    ASSERT((flags & PAL_USER) != 0);
    lock_acquire(&frame_lock);

    void *kpage = palloc_get_page(flags);
    if (kpage == NULL) {
        // 프레임 부족 시 eviction
        struct frame_entry *victim = choose_victim();
        ASSERT(victim != NULL);

        struct thread *owner = victim->owner;
        struct page *page = spt_find_in_thread(owner, victim->upage);
        ASSERT(page != NULL);

        // dirty면 스왑아웃, 아니면 file에 반영
        if (pagedir_is_dirty(owner->pagedir, victim->upage) || page->loc == PAGE_SWAP) {
            size_t slot = swap_out(victim->kpage);
            page->loc = PAGE_SWAP;
            page->swap_index = slot;
        }

        pagedir_clear_page(owner->pagedir, victim->upage);
        kpage = victim->kpage;

        list_remove(&victim->elem);
        free(victim);
    }

    struct frame_entry *f = malloc(sizeof(struct frame_entry));
    f->kpage = kpage;
    f->upage = upage;
    f->owner = thread_current();
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
