# PintOS Project 1: Threads (CS 140 / UNIST OS)

Coursework on the PintOS teaching kernel, Project 1 (threads). Solo work.

## Implemented
- Alarm clock: timer_sleep() no longer busy-waits. A thread stores wakeup_tick, joins sleep_list (kept sorted by wakeup_tick) and blocks; the timer interrupt only checks the front of the list and unblocks threads that are due (src/devices/timer.c, src/threads/thread.h).
- Priority scheduling: ready_list is kept in priority order (priority_more, list_insert_ordered) on unblock and yield, and thread_set_priority() yields when a higher-priority thread is waiting (src/threads/thread.c).

## Not implemented
- Priority donation (lock, semaphore and condition-variable wake-up order, nested donation)
- Advanced (MLFQS) scheduler and fixed-point arithmetic
- Later projects (user programs, system calls, virtual memory, file system)

## Notes
project1_design_document.txt is the design document; its sections for the unimplemented parts are blank.
The rest of src/ is the PintOS skeleton.
