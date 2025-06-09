#include "vm/page.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include <string.h>
#include "userprog/process.h"

static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

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
  ASSERT(p->loc == PAGE_FILE);  // 현재는 file-backed 페이지만 처리

  // 1. 물리 프레임 할당
  void *kpage = frame_alloc(PAL_USER, p->upage);
  if (kpage == NULL)
    return false;

  // 2. 파일에서 읽을 데이터 채우기
  off_t read_bytes = file_read_at(p->file, kpage, p->read_bytes, p->offset);
  if (read_bytes != (off_t)p->read_bytes) {
    frame_free(kpage);
    return false;
  }

  // 3. 나머지 부분은 0으로 초기화
  memset(kpage + p->read_bytes, 0, p->zero_bytes);

  // 4. 사용자 주소 공간에 매핑
  if (!install_page(p->upage, kpage, p->writable)) {
    frame_free(kpage);
    return false;
  }

  return true;
}

bool is_stack_access(void *addr, void *esp) {
  return addr >= (void *) ((uint8_t *)esp - 32) && addr < PHYS_BASE;
}

bool stack_growth(void *upage) {
  void *kpage = frame_alloc(PAL_USER | PAL_ZERO, upage);
  if (kpage == NULL)
    return false;
  return install_page(upage, kpage, true);
}