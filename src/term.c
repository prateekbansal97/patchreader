/* SIGWINCH is not in strict POSIX; ask for the BSD/GNU extras in this file only. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1

#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "util.h"

#define POLL_MS 200 /* how often a blocked read re-checks for resizes */
#define ESC_MS 50   /* how long a lone ESC waits for the rest of a sequence */

static int g_fd = -1;
static struct termios g_orig;
static struct termios g_raw;
static volatile sig_atomic_t g_screen; /* 1 while raw mode + alternate screen are active */
static volatile sig_atomic_t g_winch;  /* set by SIGWINCH and SIGCONT */

static unsigned char g_in[64];
static size_t g_inlen;

/* Only async-signal-safe calls (write, tcsetattr) may be used here and in
 * the handlers below. */
static void enter_tty(void) {
    static const char seq[] = "\x1b[?1049h\x1b[?25l";
    tcsetattr(g_fd, TCSAFLUSH, &g_raw);
    ssize_t w = write(g_fd, seq, sizeof seq - 1);
    (void)w;
    g_screen = 1;
}

static void restore_tty(void) {
    static const char seq[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
    if (!g_screen) return;
    g_screen = 0;
    ssize_t w = write(g_fd, seq, sizeof seq - 1);
    (void)w;
    tcsetattr(g_fd, TCSAFLUSH, &g_orig);
}

void term_restore(void) {
    restore_tty();
}

static void on_fatal(int sig) { /* handler is one-shot (SA_RESETHAND): re-raise to die normally */
    restore_tty();
    raise(sig);
}

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

static void install(int sig, void (*fn)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags; /* never SA_RESTART: blocked reads must wake up */
    sigaction(sig, &sa, NULL);
}

static void on_tstp(int sig) {
    restore_tty();
    raise(sig); /* default action (stop) is back in place: we sleep here until SIGCONT */
    install(SIGTSTP, on_tstp, SA_RESETHAND | SA_NODEFER);
}

static void on_cont(int sig) {
    (void)sig;
    if (g_fd >= 0) {
        enter_tty();
        g_winch = 1; /* redraw */
    }
}

bool term_init(void) {
    g_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) return false;
    if (tcgetattr(g_fd, &g_orig) != 0) {
        close(g_fd);
        g_fd = -1;
        return false;
    }

    g_raw = g_orig;
    g_raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    g_raw.c_oflag &= (tcflag_t) ~OPOST;
    g_raw.c_cflag |= CS8;
    g_raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN); /* ISIG stays on: Ctrl-C and Ctrl-Z are signals */
    g_raw.c_cc[VMIN] = 1;
    g_raw.c_cc[VTIME] = 0;

    install(SIGINT, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGTERM, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGHUP, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGQUIT, on_fatal, SA_RESETHAND | SA_NODEFER);
    /* a crash must not leave the terminal in raw mode on the alternate screen */
    install(SIGABRT, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGSEGV, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGBUS, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGFPE, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGILL, on_fatal, SA_RESETHAND | SA_NODEFER);
    install(SIGTSTP, on_tstp, SA_RESETHAND | SA_NODEFER);
    install(SIGCONT, on_cont, 0);
    install(SIGWINCH, on_winch, 0);

    pr_die_hook = term_restore; /* so die() messages land on the normal screen */
    atexit(term_restore);
    enter_tty();
    return true;
}

void term_size(int *rows, int *cols) {
    struct winsize ws;
    *rows = 24;
    *cols = 80;
    if (g_fd >= 0 && ioctl(g_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
}

void term_write(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(g_fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

Key term_read_key(void) {
    Key key;
    for (;;) {
        if (g_winch) {
            g_winch = 0;
            key.type = KEY_RESIZE;
            key.ch = 0;
            return key;
        }

        size_t used = key_parse(g_in, g_inlen, &key);
        if (used > 0) {
            memmove(g_in, g_in + used, g_inlen - used);
            g_inlen -= used;
            if (key.type == KEY_NONE) continue;
            return key;
        }
        if (g_inlen >= sizeof g_in) { /* cannot happen; never wedge on it */
            memmove(g_in, g_in + 1, --g_inlen);
            continue;
        }

        /* select, not poll: poll() does not work on tty devices on macOS */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_fd, &rfds);
        int ms = g_inlen > 0 ? ESC_MS : POLL_MS;
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
        int r = select(g_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            key.type = KEY_EOF;
            key.ch = 0;
            return key;
        }
        if (r == 0) {
            if (g_inlen > 0) { /* the sequence never finished: it was a lone ESC (or junk) */
                unsigned char first = g_in[0];
                memmove(g_in, g_in + 1, --g_inlen);
                if (first == 0x1B) {
                    key.type = KEY_ESC;
                    key.ch = 0;
                    return key;
                }
            }
            continue;
        }

        ssize_t got = read(g_fd, g_in + g_inlen, sizeof g_in - g_inlen);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) {
            key.type = KEY_EOF;
            key.ch = 0;
            return key;
        }
        g_inlen += (size_t)got;
    }
}
