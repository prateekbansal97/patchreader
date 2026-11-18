#ifndef PR_FUZZ_H
#define PR_FUZZ_H

#include <stdint.h>
#include <string.h>

#include "util.h"

/* Deterministic xorshift generator and a byte-level mutator shared by the
 * fuzz-style tests. */

static uint32_t g_rng = 12345u;

static inline uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* b = src[0..n) with 1..10 random edits (byte flips, deletions, token
 * insertions, truncation, chunk duplication). */
static inline void mutate(Buf *b, const char *src, size_t n) {
    static const char *const ins[] = {"\n",   "@@ -1,2 +1,2 @@\n", "diff --git ", "\"", "\\",
                                      "--- ", "+++ ",              "\r",          "@@@ ", "\\ No",
                                      " ",    "-",                 "+"};
    buf_clear(b);
    buf_append(b, src, n);
    uint32_t edits = 1 + rnd() % 10;
    for (uint32_t e = 0; e < edits && b->len > 0; e++) {
        size_t pos = rnd() % b->len;
        switch (rnd() % 5) {
        case 0:
            b->data[pos] = (char)(rnd() & 0xff);
            break;
        case 1: {
            size_t cnt = 1 + rnd() % 40;
            if (cnt > b->len - pos) cnt = b->len - pos;
            memmove(b->data + pos, b->data + pos + cnt, b->len - pos - cnt);
            b->len -= cnt;
            break;
        }
        case 2: {
            const char *t = ins[rnd() % (sizeof ins / sizeof ins[0])];
            size_t tl = strlen(t);
            buf_reserve(b, tl);
            memmove(b->data + pos + tl, b->data + pos, b->len - pos);
            memcpy(b->data + pos, t, tl);
            b->len += tl;
            break;
        }
        case 3:
            b->len = pos;
            break;
        default: {
            size_t cnt = 1 + rnd() % 60;
            if (cnt > b->len - pos) cnt = b->len - pos;
            char chunk[64];
            memcpy(chunk, b->data + pos, cnt);
            buf_reserve(b, cnt);
            memmove(b->data + pos + cnt, b->data + pos, b->len - pos);
            memcpy(b->data + pos, chunk, cnt);
            b->len += cnt;
        }
        }
        b->data[b->len] = '\0';
    }
}

#endif
