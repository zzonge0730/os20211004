#include "vm/swap.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "lib/kernel/bitmap.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct block *swap_block;
static struct bitmap *swap_bitmap;
struct lock swap_lock;

void swap_init(void) {
    // 락 초기화를 가장 먼저 수행하여 항상 초기화되도록 보장합니다.
    lock_init(&swap_lock);
    
    swap_block = block_get_role(BLOCK_SWAP);
    if (swap_block == NULL) {
        swap_bitmap = bitmap_create(0); // 스왑 공간 없으므로 크기 0
        if (swap_bitmap == NULL)
            PANIC("Failed to create empty swap bitmap.");
        return;
    }

    size_t slot_cnt = block_size(swap_block) / SECTORS_PER_PAGE;
    printf("SWAP AREA INITIALIZED: %d slots available.\n", slot_cnt);
    swap_bitmap = bitmap_create(slot_cnt);
    if (swap_bitmap == NULL)
        PANIC("Failed to create swap bitmap.");
}
size_t swap_out(void *kpage) {
    ASSERT(swap_block != NULL && swap_bitmap != NULL);
    lock_acquire(&swap_lock);
    size_t slot = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    if (slot == BITMAP_ERROR) {
        lock_release(&swap_lock);
        PANIC("Swap disk full!");
    }
    int i;
    for (i = 0; i < SECTORS_PER_PAGE; i++)
        block_write(swap_block, slot * SECTORS_PER_PAGE + i, (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    lock_release(&swap_lock);
    return slot;
}

void swap_in(size_t slot, void *kpage) {
    ASSERT(swap_block != NULL && swap_bitmap != NULL);
    lock_acquire(&swap_lock);
    ASSERT(bitmap_test(swap_bitmap, slot));
    int i;
    for (i = 0; i < SECTORS_PER_PAGE; i++)
        block_read(swap_block, slot * SECTORS_PER_PAGE + i, (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    bitmap_reset(swap_bitmap, slot);
    lock_release(&swap_lock);
}

void swap_free(size_t slot) {
    lock_acquire(&swap_lock);
    bitmap_reset(swap_bitmap, slot);
    lock_release(&swap_lock);
}

