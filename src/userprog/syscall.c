#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include <string.h>
#include "userprog/process.h"
#include "vm/page.h"
#include "threads/malloc.h"

// 함수 선언
void halt(void);
void exit(int status);
int write(int fd, const void *buffer, unsigned size);
bool create(const char *file, unsigned initial_size);
int open(const char *file);
void close(int fd);
int read(int fd, void *buffer, unsigned size);
int filesize(int fd);
bool remove(const char *file);
void seek(int fd, unsigned position);
unsigned tell(int fd);

void check_address(void *addr);
void check_valid_buffer(const void *buffer, unsigned size);
void check_valid_string(const char *str);
void check_syscall_args(void *esp, int num_args);
// syscall 핸들러
static void syscall_handler(struct intr_frame *);
static bool mmap_overlap(struct thread *t, void *addr, size_t size);
// 초기화
void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void check_address(void *addr) {
    if (addr == NULL || !is_user_vaddr(addr)) {
        exit(-1);
    }
}



// 페이지 단위 버퍼 검사
void check_valid_buffer(const void *buffer, unsigned size) {
    uintptr_t start = (uintptr_t) buffer;
    uintptr_t end = start + size;
    uintptr_t page_start = start & ~(PGSIZE - 1);
    uintptr_t addr;
    for (addr = page_start; addr < end; addr += PGSIZE) {
        check_address((void *)addr);
    }
}

// 문자열 검사 (문자 단위, 페이지 단위 최적화 가능)
void check_valid_string(const char *str) {
    if (str == NULL)
        exit(-1);

    uintptr_t ptr = (uintptr_t) str;
    while (true) {
        check_address((void *)ptr);
        if (*(char *)ptr == '\0')
            break;
        ptr++;
    }
}
void check_syscall_args(void *esp, int num_args) {
    size_t size = 4 * (num_args + 1); // syscall_num + args
    check_valid_buffer(esp, size);
}
static void syscall_handler(struct intr_frame *f) {
    void *esp = f->esp;

    check_syscall_args(esp, 3);

    int syscall_num = *(int *)esp;

    switch (syscall_num) {
        case SYS_HALT:
            halt();
            break;

        case SYS_EXIT:
            check_syscall_args(esp, 1);
            exit(*(int *)(esp + 4));
            break;

        case SYS_WRITE:
            check_syscall_args(esp, 3);
            {
                int fd = *(int *)(esp + 4);
                void *buffer = *(void **)(esp + 8);
                unsigned size = *(unsigned *)(esp + 12);

                check_valid_buffer(buffer, size);

                f->eax = write(fd, buffer, size);
            }
            break;

        case SYS_CREATE:
            check_syscall_args(esp, 2);
            {
                const char *file = *(const char **)(esp + 4);
                unsigned initial_size = *(unsigned *)(esp + 8);

                check_valid_string(file);

                f->eax = create(file, initial_size);
            }
            break;

        case SYS_OPEN:
            check_syscall_args(esp, 1);
            {
                const char *file = *(const char **)(esp + 4);

                check_valid_string(file);

                f->eax = open(file);
            }
            break;

        case SYS_CLOSE:
            check_syscall_args(esp, 1);
            {
                int fd = *(int *)(esp + 4);
                close(fd);
            }
            break;

        case SYS_READ:
            check_syscall_args(esp, 3);
            {
                int fd = *(int *)(esp + 4);
                void *buffer = *(void **)(esp + 8);
                unsigned size = *(unsigned *)(esp + 12);

                check_valid_buffer(buffer, size);

                f->eax = read(fd, buffer, size);
            }
            break;

        case SYS_FILESIZE:
            check_syscall_args(esp, 1);
            {
                int fd = *(int *)(esp + 4);
                f->eax = filesize(fd);
            }
            break;

        case SYS_REMOVE:
            check_syscall_args(esp, 1);
            {
                const char *file = *(const char **)(esp + 4);
                check_valid_string(file);
                f->eax = remove(file);
            }
            break;

        case SYS_SEEK:
            check_syscall_args(esp, 2);
            {
                int fd = *(int *)(esp + 4);
                unsigned pos = *(unsigned *)(esp + 8);
                seek(fd, pos);
            }
            break;

        case SYS_TELL:
            check_syscall_args(esp, 1);
            {
                int fd = *(int *)(esp + 4);
                f->eax = tell(fd);
            }
            break;

        case SYS_EXEC:
            check_syscall_args(esp, 1);
            {
                const char *cmd_line = *(const char **)(esp + 4);
                check_valid_string(cmd_line);
                f->eax = process_execute(cmd_line);
            }
            break;

        case SYS_WAIT:
            check_syscall_args(esp, 1);
            {
                tid_t pid = *(tid_t *)(esp + 4);
                f->eax = process_wait(pid);
            }
            break;
        case SYS_MMAP: {
            int fd = *(int *)(f->esp + 4);
            void *addr = *(void **)(f->esp + 8);
            f->eax = mmap (fd, addr);
            break;
        }
        case SYS_MUNMAP: {
            mapid_t mapid = *(mapid_t *)(f->esp + 4);
            munmap (mapid);
            f->eax = 0; // 반환 값 무시 (void)
            break;
        }
        default:
            exit(-1);
            break;
    }
}



// syscall 함수들
void halt(void) {
    shutdown_power_off();
}

void exit(int status) {
    struct thread *cur = thread_current();
    cur->exit_status = status;
    thread_exit();  // process_exit()에서 자원 정리 담당
}


int write(int fd, const void *buffer, unsigned size) {
    check_valid_buffer(buffer, size);

    struct thread *cur = thread_current();

    if (fd == 1) { // STDOUT
        putbuf(buffer, size);
        return size;
    }

    if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
        return -1;

    return file_write(cur->fd_table[fd], buffer, size);
}



bool create(const char *file, unsigned initial_size) {
    check_valid_string(file);
    if (file == NULL) // NULL 포인터 검사
    exit(-1);
    return filesys_create(file, initial_size);
}

int open(const char *file) {
    check_valid_string(file);
    if (file == NULL)
        exit(-1);
    struct thread *cur = thread_current();
    struct file *f = filesys_open(file);

    if (f == NULL)
        return -1;

    // 실행파일이면 fd_table에 넣지 않음
    if (strcmp(file, cur->name) == 0) {
        file_close(f);
        return -1;
    }

    int i;
    for (i = 2; i < FD_MAX; i++) {
        if (cur->fd_table[i] == NULL) {
            cur->fd_table[i] = f;
            return i;
        }
    }

    file_close(f);
    return -1;
}



void close(int fd) {
    struct thread *cur = thread_current();

    if (fd < 2 || fd >= 128)
        return;

    if (cur->fd_table[fd] != NULL) {
        file_close(cur->fd_table[fd]);
        cur->fd_table[fd] = NULL;
    }
}


int read(int fd, void *buffer, unsigned size) {
    check_valid_buffer(buffer, size);
    struct thread *cur = thread_current();

    if (fd == 0) {
        unsigned i;
        for (i = 0; i < size; i++)
            ((char *)buffer)[i] = input_getc();
        return size;
    }

    if (fd < 2 || fd >= FD_MAX || cur->fd_table[fd] == NULL)
        return -1;

    char *kbuf = palloc_get_page(0);
    if (!kbuf) return -1;

    int bytes = file_read(cur->fd_table[fd], kbuf, size);
    if (bytes > 0) {
        memcpy(buffer, kbuf, bytes);
    }

    palloc_free_page(kbuf);
    return bytes;
}

int filesize(int fd) {
    struct thread *cur = thread_current();
    if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
        return -1;

    return file_length(cur->fd_table[fd]);
}

bool remove(const char *file) {
    check_valid_string(file);
    if (file == NULL)
        exit(-1);
    return filesys_remove(file);
}

void seek(int fd, unsigned position) {
    struct thread *cur = thread_current();
    if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
        return;
    file_seek(cur->fd_table[fd], position);
}

unsigned tell(int fd) {
    struct thread *cur = thread_current();
    if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
        return -1;
    return file_tell(cur->fd_table[fd]);
}

mapid_t mmap(int fd, void *addr) {
    struct thread *cur = thread_current();
    struct file *file;
    off_t length;
    off_t offset;
    size_t read_bytes, zero_bytes;
    void *page_addr;

    if (addr == NULL || pg_ofs(addr) != 0 || fd < 2 || fd >= FD_MAX)
        return -1;

    file = file_reopen(cur->fd_table[fd]);
    if (file == NULL)
        return -1;

    length = file_length(file);
    if (length == 0) {
        file_close(file);
        return -1;
    }

    if (mmap_overlap(cur, addr, length)) {
        file_close(file);
        return -1;
    }

    for (page_addr = addr; page_addr < addr + length; page_addr += PGSIZE) {
        if (spt_find(page_addr) != NULL || page_addr >= PHYS_BASE) {
            file_close(file);
            return -1;
        }
    }

    struct mmap_entry *mmapf = malloc(sizeof(struct mmap_entry));
    if (mmapf == NULL) {
        file_close(file);
        return -1;
    }

    mmapf->id = cur->next_mapid++;
    mmapf->file = file;
    mmapf->addr = addr;
    mmapf->size = length;

    for (offset = 0; offset < length; offset += PGSIZE) {
        struct page *p = malloc(sizeof(struct page));
        if (p == NULL) {
            file_close(file);
            free(mmapf);
            return -1;
        }

        read_bytes = (length - offset) < PGSIZE ? (length - offset) : PGSIZE;
        zero_bytes = PGSIZE - read_bytes;

        p->loc = PAGE_FILE;
        p->file = file;
        p->offset = offset;
        p->upage = addr + offset;
        p->read_bytes = read_bytes;
        p->zero_bytes = zero_bytes;
        p->writable = true;
        p->owner = cur;

        if (!spt_insert(p)) {
            free(p);
            file_close(file);
            free(mmapf);
            return -1;
        }
    }

    list_push_back(&cur->mmap_list, &mmapf->elem);
    return mmapf->id;
}

// 주소 중복 검사 함수
static bool
mmap_overlap (struct thread *t, void *addr, size_t size) 
{
    struct list_elem *e;
    for (e = list_begin (&t->mmap_list); e != list_end (&t->mmap_list);
         e = list_next (e)) {
        struct mmap_entry *entry = list_entry (e, struct mmap_entry, elem);
        if (addr < entry->addr + entry->size && 
            addr + size > entry->addr) {
            return true;
        }
    }
    return false; 
}
// 매핑 검색 함수
static struct mmap_entry *
mmap_find (struct thread *t, mapid_t mapid) 
{
    struct list_elem *e;
    for (e = list_begin (&t->mmap_list); e != list_end (&t->mmap_list);
         e = list_next (e)) {
        struct mmap_entry *entry = list_entry (e, struct mmap_entry, elem);
        if (entry->id == mapid) return entry;
    }
    return NULL;
}

void munmap(mapid_t mapid) {
    struct thread *cur = thread_current();
    struct mmap_entry *entry = mmap_find(cur, mapid);
    if (entry == NULL) return;

    off_t ofs;
    for (ofs = 0; ofs < entry->size; ofs += PGSIZE) {
        void *page_addr = entry->addr + ofs;
        if (pagedir_is_dirty(cur->pagedir, page_addr)) {
            file_write_at(entry->file, page_addr,
                          MIN(PGSIZE, entry->size - ofs), ofs);
        }
        void *kpage = pagedir_get_page(cur->pagedir, page_addr);
        if (kpage != NULL)
            frame_free(kpage);
        pagedir_clear_page(cur->pagedir, page_addr);
    }

    file_close(entry->file);
    list_remove(&entry->elem);
    free(entry);
}

