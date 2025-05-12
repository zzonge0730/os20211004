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

// 함수 선언
void halt(void);
void exit(int status);
int write(int fd, const void *buffer, unsigned size);
bool create(const char *file, unsigned initial_size);
int open(const char *file);
void close(int fd);
int read(int fd, void *buffer, unsigned size);


void check_address(void *addr);
void check_valid_buffer(const void *buffer, unsigned size);
void check_valid_string(const char *str);

// syscall 핸들러
static void syscall_handler(struct intr_frame *);

// 초기화
void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void check_address(void *addr) {
    if (addr == NULL || !is_user_vaddr(addr) || pagedir_get_page(thread_current()->pagedir, addr) == NULL) {
        exit(-1);
    }
}

void check_valid_buffer(const void *buffer, unsigned size) {
    char *buf = (char *)buffer;
    unsigned i;
    for (i = 0; i < size; i++) {
        check_address(buf + i);
    }
}

void check_valid_string(const char *str) {
    const char *ptr = str;
    while (true) {
        check_address((void *)ptr);
        if (*ptr == '\0') {
            break;
        }
        ptr++;
    }
}

static void syscall_handler(struct intr_frame *f) {
    void *esp = f->esp;
    check_address(esp);

    int syscall_num = *(int *)esp;

    switch (syscall_num) {
        case SYS_HALT:
            halt();
            break;

        case SYS_EXIT:
            check_address(esp + 4);
            exit(*(int *)(esp + 4));
            break;

        case SYS_WRITE:
            check_address(esp + 4);
            check_address(esp + 8);
            check_address(esp + 12);
            {
                int fd = *(int *)(esp + 4);
                void *buffer = *(void **)(esp + 8);
                unsigned size = *(unsigned *)(esp + 12);

                // 🛡️ 버퍼 전체 검사 추가
                check_valid_buffer(buffer, size);

                f->eax = write(fd, buffer, size);
            }
            break;

        case SYS_CREATE:
            check_address(esp + 4);
            check_address(esp + 8);
            {
                const char *file = *(const char **)(esp + 4);
                unsigned initial_size = *(unsigned *)(esp + 8);

                // 🛡️ 파일명 전체 검사 추가
                check_valid_string(file);

                f->eax = create(file, initial_size);
            }
            break;

        case SYS_OPEN:
            check_address(esp + 4);
            {
                const char *file = *(const char **)(esp + 4);

                // 🛡️ 파일명 전체 검사 추가
                check_valid_string(file);

                f->eax = open(file);
            }
            break;

        case SYS_CLOSE:
            check_address(esp + 4);
            {
                int fd = *(int *)(esp + 4);
                close(fd);
            }
            break;
        case SYS_READ:
            check_address(esp + 4);
            check_address(esp + 8);
            check_address(esp + 12);
            {
                int fd = *(int *)(esp + 4);
                void *buffer = *(void **)(esp + 8);
                unsigned size = *(unsigned *)(esp + 12);

                check_valid_buffer(buffer, size); // 버퍼 전체 접근 가능성 확인

                f->eax = read(fd, buffer, size);
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
    int i;

    cur->exit_status = status;

    // 열린 파일 다 닫기
    for (i = 2; i < 128; i++) {
        if (cur->fd_table[i] != NULL) {
            file_close(cur->fd_table[i]);
            cur->fd_table[i] = NULL;
        }
    }
    printf("%s: exit(%d)\n", cur->name, status);
    thread_exit();
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
    if (file == NULL) // NULL 포인터 검사
    exit(-1);
    struct thread *cur = thread_current();
    struct file *f = filesys_open(file);

    if (f == NULL)
        return -1;

    int i;
    for (i = 2; i < 128; i++) {
        if (cur->fd_table[i] == NULL) {
            cur->fd_table[i] = f;
            return i;
        }
    }

    // fd_table이 꽉 찼으면 열었던 파일 닫기
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

