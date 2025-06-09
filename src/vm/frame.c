#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <list.h>
#include <debug.h>

/* Frame table (전역) */
static struct list frame_table;
static struct lock frame_lock;

void
frame_table_init(void) {
  list_init(&frame_table);
  lock_init(&frame_lock);
}

void *
frame_alloc(enum palloc_flags flags, void *upage) {
  ASSERT((flags & PAL_USER) != 0);  // 반드시 유저 영역
  lock_acquire(&frame_lock);

  void *kpage = palloc_get_page(flags);
  if (kpage == NULL) {
    // 나중에: 교체 알고리즘 실행
    lock_release(&frame_lock);
    return NULL;
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
