#ifndef HAKU_TERMIOS_H
#define HAKU_TERMIOS_H

#include <haku/abi.h>

typedef haku_termios_t termios_t;

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* Line discipline flags the console reports. */
#define IGNBRK  HAKU_IGNBRK
#define BRKINT  HAKU_BRKINT
#define IGNPAR  HAKU_IGNPAR
#define PARMRK  HAKU_PARMRK
#define INPCK   HAKU_INPCK
#define ISTRIP  HAKU_ISTRIP
#define INLCR   HAKU_INLCR
#define IGNCR   HAKU_IGNCR
#define ICRNL   HAKU_ICRNL
#define IXON    HAKU_IXON
#define OPOST   HAKU_OPOST
#define ONLCR   HAKU_ONLCR
#define ISIG    HAKU_ISIG
#define ICANON  HAKU_ICANON
#define ECHO    HAKU_ECHO
#define ECHOE   HAKU_ECHOE
#define ECHOK   HAKU_ECHOK
#define ECHONL  HAKU_ECHONL

#define VINTR   HAKU_VINTR
#define VQUIT   HAKU_VQUIT
#define VERASE  HAKU_VERASE
#define VKILL   HAKU_VKILL
#define VEOF    HAKU_VEOF
#define VTIME   HAKU_VTIME
#define VMIN    HAKU_VMIN
#define VSTART  HAKU_VSTART
#define VSTOP   HAKU_VSTOP
#define VSUSP   HAKU_VSUSP
#define VEOL    HAKU_VEOL
#define VWERASE HAKU_VWERASE
#define VLNEXT  HAKU_VLNEXT

#define TCGETS     HAKU_TCGETS
#define TCSETS     HAKU_TCSETS
#define TCSETSW    HAKU_TCSETSW
#define TCSETSF    HAKU_TCSETSF
#define TCSBRK     HAKU_TCSBRK
#define TCFLSH     HAKU_TCFLSH
#define TIOCGPGRP  HAKU_TIOCGPGRP
#define TIOCSPGRP  HAKU_TIOCSPGRP
#define TIOCGWINSZ HAKU_TIOCGWINSZ
#define FIONREAD   HAKU_FIONREAD
#define TIOCNOTTY  HAKU_TIOCNOTTY

int ioctl(int fd, unsigned int request, void *argument);
int tcgetattr(int fd, termios_t *attributes);
int tcsetattr(int fd, int action, const termios_t *attributes);
int tcgetpgrp(int fd);
int tcsetpgrp(int fd, int pgrp);
int isatty(int fd);

#endif
