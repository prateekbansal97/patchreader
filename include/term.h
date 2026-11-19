#ifndef PR_TERM_H
#define PR_TERM_H

#include <stdbool.h>
#include <stddef.h>

#include "input.h"

/* Puts the controlling terminal (/dev/tty) into raw mode on the alternate
 * screen with the cursor hidden. Returns false if there is no terminal.
 * The terminal is restored automatically on exit(), die(), SIGINT, SIGTERM,
 * SIGHUP, SIGQUIT and Ctrl-Z (and re-entered on fg). */
bool term_init(void);

/* Leaves the alternate screen and restores the saved terminal settings.
 * Safe to call more than once and from signal handlers. */
void term_restore(void);

/* Current size in character cells; falls back to 24x80. */
void term_size(int *rows, int *cols);

/* Blocks for the next key. Returns KEY_RESIZE after a window size change
 * (or after resuming from Ctrl-Z) and KEY_EOF if the terminal is gone. */
Key term_read_key(void);

/* Writes all n bytes to the terminal. */
void term_write(const char *s, size_t n);

#endif