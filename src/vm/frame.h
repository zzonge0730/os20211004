#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdbool.h>
#include "threads/thread.h"  // struct thread 필요
#include "threads/palloc.h"

/* 프레임 테이블의 엔트리 구조 */
struct frame_entry {
  void *kpage;              // 커널 가상 주소
  void *upage;              // 유저 가상 주소
  struct thread *owner;     // 프레임을 소유한 쓰레드
  struct list_elem elem;    // 프레임 테이블용 list_elem
  bool pinned;
};

/* 함수 프로토타입 선언 */
void frame_table_init(void);
void *frame_alloc(enum palloc_flags flags, void *upage);
void frame_free(void *kpage);
void frame_pin(void *kpage);
void frame_unpin(void *kpage);
#endif /* VM_FRAME_H */
