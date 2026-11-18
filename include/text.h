#ifndef PR_TEXT_H
#define PR_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define TAB_WIDTH 4

/* Decodes one UTF-8 sequence from s (n >= 1 bytes available). Invalid,
 * overlong, surrogate or truncated input yields U+FFFD and consumes one
 * byte. *used is always in [1, n]. */
uint32_t utf8_decode(const char *s, size_t n, size_t *used);

/* Appends the UTF-8 encoding of cp; invalid code points become U+FFFD. */
void utf8_encode(Buf *out, uint32_t cp);

/* Terminal columns taken by a code point: 0 (combining), 1 or 2 (wide). */
int cp_width(uint32_t cp);

/* Columns taken by the text with tabs expanded to TAB_WIDTH stops and every
 * control byte shown as a one-column symbol. */
size_t text_width(const char *s, size_t n);

/* Appends the part of the text covering display columns [skip, skip+cols),
 * made safe for a terminal: tabs become spaces, control bytes become
 * visible symbols (ESC -> U+241B), invalid UTF-8 becomes U+FFFD, and a wide
 * character cut by either edge is shown as spaces. Returns the number of
 * columns appended (<= cols). */
size_t text_window(Buf *out, const char *s, size_t n, size_t skip, size_t cols);

#endif