#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void page_destroy(struct hash_elem *e, void *aux UNUSED);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute(const char *cmd_line) {
    static bool frame_table_initialized = false;
    if (!frame_table_initialized) {
        frame_table_init();
        frame_table_initialized = true;
    }
    if (cmd_line == NULL) return TID_ERROR;
    char *fn_copy = palloc_get_page(0);
    if (fn_copy == NULL) return TID_ERROR;
    strlcpy(fn_copy, cmd_line, PGSIZE);

    tid_t tid = thread_create(fn_copy, PRI_DEFAULT, start_process, fn_copy);
    if (tid == TID_ERROR) {
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }

    // child_status 구조체 메모리 확보 및 초기화
    struct child_status *cs = malloc(sizeof(struct child_status));
    if (cs == NULL) return TID_ERROR;

    memset(cs, 0, sizeof(struct child_status));  // 모든 필드 안전 초기화
    cs->tid = tid;
    sema_init(&cs->sema, 0);


    // 자식 리스트에 추가
    list_push_back(&thread_current()->children, &cs->elem);

    // 자식 스레드 포인터 가져오기
    struct thread *child = get_thread_by_tid(tid);
    if (child == NULL) {
        return -1;
    }
    spt_init(child);
    child->self_status = cs;
    child->parent_thread = thread_current();

    // exec 성공 여부를 기다림
    sema_down(&child->exec_sema);
    if (!cs->load_success) {
        list_remove(&cs->elem);
        free(cs);
        return -1;
    }

    return tid;
}


static void
start_process(void *cmd_line_) {
    char *cmd_line = cmd_line_;
    if (cmd_line == NULL || cmd_line[0] == '\0') {
        thread_exit();
    }    
    struct intr_frame if_;
    bool success;
    struct thread *cur = thread_current();
    void *esp;
    int i;

    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;

    // 인자 파싱
    char *argv[128];
    int argc = 0;
    char *token, *save_ptr;
    for (token = strtok_r(cmd_line, " ", &save_ptr); token != NULL;
         token = strtok_r(NULL, " ", &save_ptr)) {
        argv[argc++] = token;
    }
    argv[argc] = NULL;

    strlcpy(cur->name, argv[0], sizeof cur->name);

    // 실제 로딩 수행
    success = load(argv[0], &if_.eip, &if_.esp);

    // 부모에게 로딩 성공 여부 전달
    cur->self_status->load_success = success;
    sema_up(&cur->exec_sema);

    if (!success) {
        palloc_free_page(cmd_line);
        thread_exit();
    }

    // 사용자 스택 구성
    esp = if_.esp;
    char *arg_addr[128];

    for (i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        esp -= len;
        memcpy(esp, argv[i], len);
        arg_addr[i] = (char *)esp;
    }

    uintptr_t align = (uintptr_t)esp % 4;
    if (align != 0) {
        esp -= align;
        memset(esp, 0, align);
    }

    esp -= sizeof(char *);
    *(char **)esp = NULL;

    for (i = argc - 1; i >= 0; i--) {
        esp -= sizeof(char *);
        *(char **)esp = arg_addr[i];
    }

    char **argv_start = (char **)esp;

    esp -= sizeof(char **);
    *(char ***)esp = argv_start;

    esp -= sizeof(int);
    *(int *)esp = argc;

    esp -= sizeof(void *);
    *(void **)esp = NULL;

    if_.esp = esp;

    palloc_free_page(cmd_line);

    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");

    NOT_REACHED();
}




/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid) {
    struct thread *cur = thread_current();
    struct list_elem *e;
    struct child_status *cs = NULL;

    // 부모의 children 리스트에서 자식 상태 찾기
    for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e)) {
        struct child_status *entry = list_entry(e, struct child_status, elem);
        if (entry->tid == child_tid) {
            cs = entry;
            break;
        }
    }

    if (cs == NULL || cs->has_been_waited)
        return -1;

    cs->has_been_waited = true;

    if (!cs->has_exited)
        sema_down(&cs->sema);

    int status = cs->exit_status;
    list_remove(&cs->elem);
    free(cs);
    return status;
}


/* Free the current process's resources. */
void process_exit(void) {
    struct thread *cur = thread_current();

    // 1. mmap 리스트 해제 (munmap 호출)
    while (!list_empty(&cur->mmap_list)) {
        struct list_elem *e = list_pop_front(&cur->mmap_list);
        struct mmap_entry *entry = list_entry(e, struct mmap_entry, elem);
        do_munmap(entry); // 리스트를 건드리지 않는 헬퍼 함수 호출
    }
    // 2. supplemental page table 전부 해제
    hash_clear(&cur->spt, page_destroy);

    // 3. 열린 파일들 모두 닫기 (fd_table 순회)
    int fd;
    for (fd = 2; fd < FD_MAX; fd++) {
        if (cur->fd_table[fd] != NULL) {
            if (cur->fd_table[fd] == cur->executable) {
                // 실행파일은 아래에서 따로 닫으므로 여기선 그냥 NULL 처리
                cur->fd_table[fd] = NULL;
            } else {
                file_close(cur->fd_table[fd]);
                cur->fd_table[fd] = NULL;
            }
        }
    }

    // 4. 실행 중인 파일 닫기 (allow_write 포함)
    if (cur->executable != NULL) {
        file_allow_write(cur->executable);
        file_close(cur->executable);
        cur->executable = NULL;
    }

    // 5. 페이지 디렉토리 해제
    uint32_t *pd = cur->pagedir;
    if (pd != NULL) {
        cur->pagedir = NULL;
        pagedir_activate(NULL);
        pagedir_destroy(pd);
    }

    // 6. 부모에게 종료 상태 전달
    if (cur->self_status != NULL) {
        cur->self_status->exit_status = cur->exit_status;
        cur->self_status->has_exited = true;
        sema_up(&cur->self_status->sema);
    }
    sema_up(&cur->wait_sema);

    printf("%s: exit(%d)\n", cur->name, cur->exit_status);
}
static void page_destroy(struct hash_elem *e, void *aux UNUSED) {
    struct page *p = hash_entry(e, struct page, elem);
    free(p);
}


/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  file_deny_write (file);
  t->executable = file;
  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (!success) {
      file_close(file);
  }  
  return success;
}

/* load() helpers. */

bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      // 페이지 단위로 나눔
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      // 페이지 정보 구조체 생성 및 초기화
      struct page *p = malloc(sizeof(struct page));
      if (p == NULL)
        return false;

      p->loc = PAGE_FILE;
      p->file = file;
      p->offset = ofs;
      p->upage = upage;
      p->read_bytes = page_read_bytes;
      p->zero_bytes = page_zero_bytes;
      p->writable = writable;
      p->owner = thread_current();

      if (!spt_insert(p)) {
        free(p);
        return false;
      }

      // 다음 페이지로 이동
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += page_read_bytes;
    }

  return true;
}


/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  if (stack_growth(((uint8_t *) PHYS_BASE) - PGSIZE)) 
    {
      *esp = PHYS_BASE;
      return true;
    }
  return false;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
