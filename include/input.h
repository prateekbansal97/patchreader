#ifndef PR_INPUT_H
#define PR_INPUT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    KEY_NONE, /* unrecognised input (never returned by term_read_key) */
    KEY_CHAR, /* a printable character, in Key.ch */
    KEY_CTRL, /* Ctrl + letter, letter in Key.ch ('a'..'z') */
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PGUP,
    KEY_PGDN,
    KEY_HOME,
    KEY_END,
    KEY_DEL,
    KEY_TAB,
    KEY_BTAB, /* Shift-Tab */
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_ESC,
    KEY_RESIZE, /* the terminal changed size */
    KEY_EOF     /* the terminal went away */
} KeyType;

typedef struct {
    KeyType type;
    uint32_t ch;
} Key;

/* Decodes one key from the front of in[0..n).
 * Returns the number of bytes consumed, or 0 when the bytes so far are the
 * start of a longer sequence (the caller should read more, or time out).
 * Unrecognised sequences are consumed and reported as KEY_NONE. */
size_t key_parse(const unsigned char *in, size_t n, Key *out);

/* Human-readable name, e.g. "Up", "Ctrl-d", "'q'", "U+00E9". */
void key_describe(Key k, char *out, size_t n);

#endif