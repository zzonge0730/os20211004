#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"

static void syscall_handler (struct intr_frame *);
bool create(const char *file, unsigned initial_size);
int open(const char *file);
void close(int fd);
int write(int fd, const void *buffer, unsigned size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  int syscall_num;

  if (f == NULL || f->esp == NULL)
    thread_exit();

  syscall_num = *(int *)(f->esp);

  switch (syscall_num) {
    case SYS_HALT:
      halt();
      break;

    case SYS_EXIT:
      {
        int status = *((int *)f->esp + 1);
        exit(status);
      }
      break;

    default:
      printf("[ERROR] Unknown system call number: %d\n", syscall_num);
      thread_exit();
      break;
  }
}

/* Terminates Pintos. */
void
halt(void)
{
  shutdown_power_off();
}

/* Terminates the current user program, returning status to the kernel. */
void
exit(int status)
{
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_exit();
}