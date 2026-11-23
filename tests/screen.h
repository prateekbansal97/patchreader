#ifndef PR_SCREEN_H
#define PR_SCREEN_H

#include <stdlib.h>

#include "text.h"
#include "util.h"

/* A rendered frame split back into screen rows. raw keeps our escape
 * sequences, plain has them removed. ok is cleared if the frame holds
 * anything but cursor moves (ESC[r;1H), SGR (ESC[..m) and line erase
 * (ESC[2K). */
#define SCR_MAXR 64

typedef struct {
    Buf raw[SCR_MAXR];
    Buf plain[SCR_MAXR];
    int seen[SCR_MAXR];
    int ok;
} Scr;

static inline void scr_free(Scr *s) {
    for (int i = 0; i < SCR_MAXR; i++) {
        buf_free(&s->raw[i]);
        buf_free(&s->plain[i]);
    }
}

static inline void scr_parse(Scr *s, const Buf *f) {
    for (int i = 0; i < SCR_MAXR; i++) {
        buf_init(&s->raw[i]);
        buf_init(&s->plain[i]);
        s->seen[i] = 0;
    }
    s->ok = 1;
    int cur = -1;
    size_t i = 0;
    while (i < f->len) {
        unsigned char c = (unsigned char)f->data[i];
        if (c == 0x1b) {
            if (i + 1 >= f->len || f->data[i + 1] != '[') { s->ok = 0; return; }
            size_t j = i + 2;
            while (j < f->len && ((f->data[j] >= '0' && f->data[j] <= '9') || f->data[j] == ';')) j++;
            if (j >= f->len) { s->ok = 0; return; }
            char fin = f->data[j];
            if (fin == 'H') {
                int row = atoi(f->data + i + 2);
                if (row < 1 || row > SCR_MAXR) { s->ok = 0; return; }
                cur = row - 1;
                s->seen[cur] = 1;
            } else if (fin == 'm' || fin == 'K') {
                if (cur < 0) { s->ok = 0; return; }
                buf_append(&s->raw[cur], f->data + i, j + 1 - i);
            } else {
                s->ok = 0;
                return;
            }
            i = j + 1;
            continue;
        }
        if (cur < 0) { s->ok = 0; return; }
        buf_putc(&s->raw[cur], (char)c);
        buf_putc(&s->plain[cur], (char)c);
        i++;
    }
}

/* Every screen row is painted and, unless erased, exactly cols wide. */
static inline int scr_geometry_ok(const Scr *s, int rows, int cols) {
    if (!s->ok) return 0;
    for (int r = 1; r <= rows; r++) {
        if (!s->seen[r - 1]) return 0;
        const Buf *p = &s->plain[r - 1];
        if (p->len == 0) continue;
        if (text_width(p->data, p->len) != (size_t)cols) return 0;
    }
    return 1;
}

#endif
