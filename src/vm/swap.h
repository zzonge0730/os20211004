#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include <stdbool.h>

void swap_init(void);
size_t swap_out(void *kpage);      // 프레임을 스왑 아웃할 때
void swap_in(size_t index, void *kpage);  // 스왑 슬롯에서 프레임 복원
void swap_free(size_t index);      // 스왑 슬롯 반환

#endif
