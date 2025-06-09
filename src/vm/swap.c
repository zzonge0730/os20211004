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

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct block *swap_block;
static struct bitmap *swap_bitmap;
static struct lock swap_lock;

void swap_init(void) {
    swap_block = block_get_role(BLOCK_SWAP);
    size_t slot_cnt = block_size(swap_block) / SECTORS_PER_PAGE;
    swap_bitmap = bitmap_create(slot_cnt);
    lock_init(&swap_lock);
}
size_t swap_out(void *kpage) {
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

