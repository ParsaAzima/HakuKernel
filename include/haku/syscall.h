#ifndef HAKU_SYSCALL_H
#define HAKU_SYSCALL_H

#include <haku/abi.h>

int haku_syscall0(int number);
int haku_syscall1(int number, int a);
int haku_syscall2(int number, int a, int b);
int haku_syscall3(int number, int a, int b, int c);
int haku_syscall4(int number, int a, int b, int c, int d);
int haku_syscall5(int number, int a, int b, int c, int d, int e);
int haku_syscall6(int number, int a, int b, int c, int d, int e, int f);
/* clone(2) wrapper: a thread that shares the caller's address space and
 * descriptors, starts on stack_top and runs fn(arg). Implemented in
 * syscall.asm because the child has to leave the syscall on its own stack. */
int haku_thread_create(int (*fn)(void *), void *arg, void *stack_top);
int haku_errno_check(int result);

#endif
