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
static struct lock filesys_lock;
// 초기화
void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
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

    // 먼저 시스템콜 번호만 검사
    check_address(esp);
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

        case SYS_MMAP:
            check_syscall_args(esp, 2); // mmap은 2개 인자
            {
                int fd = *(int *)(esp + 4);
                void *addr = *(void **)(esp + 8);
                
                // fd_table 유효성 검사 추가
                if (fd < 2 || fd >= FD_MAX || thread_current()->fd_table[fd] == NULL) {
                    f->eax = -1;
                    break;
                }
                
                f->eax = mmap(fd, addr);
            }
            break;

        case SYS_MUNMAP:
            check_syscall_args(esp, 1); // munmap은 1개 인자
            {
                mapid_t mapid = *(mapid_t *)(esp + 4);
                munmap(mapid);
                f->eax = 0;
            }
            break;

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
    // STDOUT은 파일 시스템 락과 무관하므로 먼저 처리
    if (fd == 1) {
        putbuf(buffer, size);
        return size;
    }

    check_valid_buffer(buffer, size);
    struct thread *cur = thread_current();
    int bytes_written = -1; // 결과 저장 변수

    lock_acquire(&filesys_lock);
    if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
        bytes_written = file_write(cur->fd_table[fd], buffer, size);
    }
    lock_release(&filesys_lock);

    return bytes_written;
}



bool create(const char *file, unsigned initial_size) {
    check_valid_string(file);
    if (file == NULL) exit(-1);

    lock_acquire(&filesys_lock);
    bool success = filesys_create(file, initial_size);
    lock_release(&filesys_lock);

    return success;
}

int open(const char *file) {
    check_valid_string(file);
    if (file == NULL) exit(-1);

    lock_acquire(&filesys_lock);
    struct file *f = filesys_open(file);
    if (f == NULL) {
        lock_release(&filesys_lock);
        return -1;
    }

    struct thread *cur = thread_current();
    int fd = -1;
    int i;
    for (i = 2; i < FD_MAX; i++) {
        if (cur->fd_table[i] == NULL) {
            cur->fd_table[i] = f;
            fd = i;
            break;
        }
    }

    if (fd == -1) { // fd table is full
        file_close(f);
    }
    
    lock_release(&filesys_lock);
    return fd;
}

void close(int fd) {
    // fd 유효성 검사는 락을 잡기 전에 해도 안전합니다.
    if (fd < 2 || fd >= FD_MAX) {
        return;
    }
    
    lock_acquire(&filesys_lock);
    struct thread *cur = thread_current();
    if (cur->fd_table[fd] != NULL) {
        file_close(cur->fd_table[fd]);
        cur->fd_table[fd] = NULL;
    }
    lock_release(&filesys_lock);
}
int read(int fd, void *buffer, unsigned size) {
    if (fd == 0) { // STDIN
        unsigned i;
        for (i = 0; i < size; i++)
            ((char *)buffer)[i] = input_getc();
        return size;
    }
    
    check_valid_buffer(buffer, size);
    struct thread *cur = thread_current();
    int bytes_read = -1;

    lock_acquire(&filesys_lock);
    if (fd >= 2 && fd < FD_MAX && cur->fd_table[fd] != NULL) {
        bytes_read = file_read(cur->fd_table[fd], buffer, size);
    }
    lock_release(&filesys_lock);
    
    return bytes_read;
}

int filesize(int fd) {
    if (fd < 2 || fd >= FD_MAX) {
        return -1;
    }

    lock_acquire(&filesys_lock);
    struct thread *cur = thread_current();
    struct file *f = cur->fd_table[fd];
    int length = -1;

    if (f != NULL) {
        length = file_length(f);
    }
    
    lock_release(&filesys_lock);
    return length;
}

bool remove(const char *file) {
    check_valid_string(file);
    if (file == NULL) {
        exit(-1);
    }

    lock_acquire(&filesys_lock);
    bool success = filesys_remove(file);
    lock_release(&filesys_lock);

    return success;
}

void seek(int fd, unsigned position) {
    if (fd < 2 || fd >= FD_MAX) {
        return;
    }

    lock_acquire(&filesys_lock);
    struct thread *cur = thread_current();
    struct file *f = cur->fd_table[fd];

    if (f != NULL) {
        file_seek(f, position);
    }

    lock_release(&filesys_lock);
}

unsigned tell(int fd) {
    if (fd < 2 || fd >= FD_MAX) {
        return -1;
    }

    lock_acquire(&filesys_lock);
    struct thread *cur = thread_current();
    struct file *f = cur->fd_table[fd];
    unsigned position = -1; // tell은 실패 시 unsigned -1을 반환할 수 있음

    if (f != NULL) {
        position = file_tell(f);
    }

    lock_release(&filesys_lock);
    return position;
}

mapid_t mmap(int fd, void *addr) {
    struct thread *cur = thread_current();
    struct file *file;
    off_t length;
    off_t offset;
    size_t read_bytes, zero_bytes;
    void *page_addr;

    // 기본 유효성 검사
    if (addr == NULL || pg_ofs(addr) != 0 || fd < 2 || fd >= FD_MAX)
        return -1;

    // fd_table 검사 (이미 syscall_handler에서 했지만 안전을 위해)
    if (cur->fd_table[fd] == NULL)
        return -1;

    // 파일을 다시 열어서 독립적인 파일 디스크립터 생성
    file = file_reopen(cur->fd_table[fd]);
    if (file == NULL)
        return -1;

    length = file_length(file);
    if (length == 0) {
        file_close(file);
        return -1;
    }

    // 기존 매핑과 중복 검사
    if (mmap_overlap(cur, addr, length)) {
        file_close(file);
        return -1;
    }

    // 각 페이지별로 더 철저한 검사
    for (page_addr = addr; page_addr < addr + length; page_addr += PGSIZE) {
        // supplemental page table에 이미 존재하는지 검사
        if (spt_find(page_addr) != NULL) {
            file_close(file);
            return -1;
        }
        // 커널 공간인지 검사
        if (page_addr >= PHYS_BASE) {
            file_close(file);
            return -1;
        }
        // 이미 매핑된 페이지인지 검사
        if (pagedir_get_page(cur->pagedir, page_addr) != NULL) {
            file_close(file);
            return -1;
        }
    }

    // mmap_entry 생성
    struct mmap_entry *mmapf = malloc(sizeof(struct mmap_entry));
    if (mmapf == NULL) {
        file_close(file);
        return -1;
    }

    mmapf->id = cur->next_mapid++;
    mmapf->file = file;
    mmapf->addr = addr;
    mmapf->size = length;

    // 각 페이지별로 supplemental page table 엔트리 생성
    for (offset = 0; offset < length; offset += PGSIZE) {
        struct page *p = malloc(sizeof(struct page));
        if (p == NULL) {
            // 실패 시 이미 생성된 페이지들 정리
            off_t cleanup_offset;
            for (cleanup_offset = 0; cleanup_offset < offset; cleanup_offset += PGSIZE) {
                struct page *cleanup_page = spt_find(addr + cleanup_offset);
                if (cleanup_page != NULL) {
                    spt_remove(cleanup_page);
                    free(cleanup_page);
                }
            }
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
            // 실패 시 현재 페이지와 이미 생성된 페이지들 정리
            free(p);
            off_t cleanup_offset;
            for (cleanup_offset = 0; cleanup_offset < offset; cleanup_offset += PGSIZE) {
                struct page *cleanup_page = spt_find(addr + cleanup_offset);
                if (cleanup_page != NULL) {
                    spt_remove(cleanup_page);
                    free(cleanup_page);
                }
            }
            file_close(file);
            free(mmapf);
            return -1;
        }
    }

    // mmap_list에 추가
    list_push_back(&cur->mmap_list, &mmapf->elem);
    return mmapf->id;
}


// 주소 중복 검사 함수
static bool
mmap_overlap(struct thread *t, void *addr, size_t size) {
    void *end_addr = addr + size;
    
    // Check against existing mmap entries
    struct list_elem *e;
    for (e = list_begin(&t->mmap_list); e != list_end(&t->mmap_list);
         e = list_next(e)) {
        struct mmap_entry *entry = list_entry(e, struct mmap_entry, elem);
        if (addr < entry->addr + entry->size && end_addr > entry->addr) {
            return true;
        }
    }
    
    // Check against code and data segments (typically 0x08048000 to stack)
    // This is a simplified check - you may need to store segment bounds
    if (addr < (void *)0x08048000 + 0x1000000) { // Rough code/data area
        return true;
    }
    
    // Check against stack area (typically near PHYS_BASE)
    if (end_addr > PHYS_BASE - 0x800000) { // 8MB stack limit
        return true;
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
void do_munmap(struct mmap_entry *entry) {
    struct thread *cur = thread_current();

    // 1. 매핑된 모든 페이지를 순회하며 자원 해제
    off_t ofs;
    for (ofs = 0; ofs < entry->size; ofs += PGSIZE) {
        void *page_addr = entry->addr + ofs;
        
        struct page *p = spt_find(page_addr);
        if (p != NULL) {
            void *kpage = pagedir_get_page(cur->pagedir, page_addr);
            // dirty bit가 켜져 있으면 파일에 다시 쓰기
            if (kpage != NULL && pagedir_is_dirty(cur->pagedir, page_addr)) {
                // MIN 매크로가 없다면 (a < b ? a : b) 로 대체
                size_t write_bytes = (p->read_bytes < PGSIZE) ? p->read_bytes : PGSIZE;
                file_write_at(p->file, kpage, write_bytes, p->offset);
            }
            // 페이지 테이블에서 매핑 해제 및 프레임 반납
            if (kpage != NULL) {
                frame_free(kpage);
                pagedir_clear_page(cur->pagedir, page_addr);
            }

            // supplemental page table에서 삭제
            hash_delete(&cur->spt, &p->elem);
            free(p);
        }
    }

    // 2. 파일 닫기 및 mmap_entry 메모리 해제
    file_close(entry->file);
    free(entry);
}
void munmap(mapid_t mapid) {
    struct thread *cur = thread_current();
    struct mmap_entry *entry = mmap_find(cur, mapid);
    
    // 해당 mapid를 찾지 못하면 아무것도 하지 않음
    if (entry == NULL) {
        return;
    }

    // 1. 리스트에서 먼저 제거
    list_remove(&entry->elem);

    // 2. 실제 자원 해제는 헬퍼 함수에 위임
    do_munmap(entry);
}
