#ifndef HAKU_FCNTL_H
#define HAKU_FCNTL_H

#include <haku/abi.h>

#define O_RDONLY HAKU_O_RDONLY
#define O_WRONLY HAKU_O_WRONLY
#define O_RDWR   HAKU_O_RDWR
#define O_ACCMODE HAKU_O_ACCMODE
#define O_CREAT  HAKU_O_CREAT
#define O_EXCL   HAKU_O_EXCL
#define O_TRUNC  HAKU_O_TRUNC
#define O_APPEND HAKU_O_APPEND
#define O_NOCTTY HAKU_O_NOCTTY
#define O_NONBLOCK HAKU_O_NONBLOCK
#define O_LARGEFILE HAKU_O_LARGEFILE
#define O_DIRECTORY HAKU_O_DIRECTORY
#define O_NOFOLLOW HAKU_O_NOFOLLOW
#define O_CLOEXEC HAKU_O_CLOEXEC

#define F_DUPFD        HAKU_F_DUPFD
#define F_GETFD        HAKU_F_GETFD
#define F_SETFD        HAKU_F_SETFD
#define F_GETFL        HAKU_F_GETFL
#define F_SETFL        HAKU_F_SETFL
#define F_GETLK        HAKU_F_GETLK
#define F_SETLK        HAKU_F_SETLK
#define F_SETLKW       HAKU_F_SETLKW
#define F_DUPFD_CLOEXEC HAKU_F_DUPFD_CLOEXEC
#define FD_CLOEXEC     HAKU_FD_CLOEXEC

#define F_RDLCK HAKU_F_RDLCK
#define F_WRLCK HAKU_F_WRLCK
#define F_UNLCK HAKU_F_UNLCK

#define AT_FDCWD       HAKU_AT_FDCWD
#define AT_REMOVEDIR   HAKU_AT_REMOVEDIR
#define AT_SYMLINK_NOFOLLOW HAKU_AT_SYMLINK_NOFOLLOW

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
/* The rest of the *at family Haku implements. A relative name resolves
 * against dirfd, or against the working directory for AT_FDCWD. */
int unlinkat(int dirfd, const char *path, int flags);
int mkdirat(int dirfd, const char *path, int mode);
int renameat(int olddirfd, const char *old_path, int newdirfd,
             const char *new_path);
int faccessat(int dirfd, const char *path, int mode, int flags);
/* Only F_DUPFD, F_GETFD/F_SETFD, F_GETFL/F_SETFL and the lock commands
 * are meaningful; record locks are accepted as no-ops. */
int fcntl(int fd, int command, int argument);

#endif
