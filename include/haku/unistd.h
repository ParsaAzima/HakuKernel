#ifndef HAKU_UNISTD_H
#define HAKU_UNISTD_H

#include <haku/types.h>
#include <haku/abi.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int old_fd, int new_fd);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int mkdir(const char *path);
int rmdir(const char *path);
/* Symbolic links, which only the RAM filesystem keeps. */
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buffer, size_t size);
/* Working directory and permission probing. */
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
int access(const char *path, int mode);
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
/* Length changes, pipes, and descriptor duplication. */
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int dup3(int old_fd, int new_fd, int flags);
int sched_yield(void);
/* Fill buffer with pseudo-random bytes. */
int getrandom(void *buffer, size_t length, unsigned int flags);
#define GRND_NONBLOCK HAKU_GRND_NONBLOCK
#define GRND_RANDOM   HAKU_GRND_RANDOM
/* Process name (prctl PR_SET_NAME/PR_GET_NAME). */
int prctl(int option, unsigned int arg2, unsigned int arg3,
          unsigned int arg4, unsigned int arg5);
#define PR_SET_NAME HAKU_PR_SET_NAME
#define PR_GET_NAME HAKU_PR_GET_NAME
#define PR_SET_PDEATHSIG HAKU_PR_SET_PDEATHSIG
#define PR_GET_PDEATHSIG HAKU_PR_GET_PDEATHSIG
#define PR_SET_DUMPABLE HAKU_PR_SET_DUMPABLE
#define PR_GET_DUMPABLE HAKU_PR_GET_DUMPABLE
#define PR_SET_NO_NEW_PRIVS HAKU_PR_SET_NO_NEW_PRIVS
#define PR_GET_NO_NEW_PRIVS HAKU_PR_GET_NO_NEW_PRIVS
int personality(unsigned int persona);
#define PER_LINUX HAKU_PER_LINUX
#define PER_QUERY HAKU_PER_QUERY
int getpid(void);
int getppid(void);
/* Process groups and sessions: a process is its own group and session,
 * so each of these reports or accepts the caller's pid. */
int getpgrp(void);
int getpgid(int pid);
int setpgid(int pid, int pgid);
int setsid(void);
int getsid(int pid);
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
int fork(void);
/* vfork(2): the child runs in the caller's address space and on its stack
 * until execve/_exit; the parent resumes with the child pid. Implemented in
 * syscall.asm so the pending return address is not left where the child
 * would overwrite it. */
int vfork(void);
int waitpid(int pid, int *status, int options);
int spawn(const char *path);
int execve(const char *path, char *const argv[], char *const envp[]);
int getusage(haku_usage_t *usage);
int gettimeofday(haku_timeval_t *time, void *timezone);
int nanosleep(const haku_timespec_t *request, haku_timespec_t *remaining);
unsigned int sleep(unsigned int seconds);
void *sbrk(int increment);
int brk(void *address);
__declspec(noreturn) void _exit(int status);

#endif
