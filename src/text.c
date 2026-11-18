#include "text.h"

#include <stdbool.h>

uint32_t utf8_decode(const char *s, size_t n, size_t *used) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[0];
    *used = 1;
    if (c < 0x80) return c;

    size_t need;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1Fu;
        min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0Fu;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07u;
        min = 0x10000;
    } else {
        return 0xFFFD;
    }
    if (n < need + 1) return 0xFFFD;
    for (size_t k = 1; k <= need; k++) {
        if ((u[k] & 0xC0u) != 0x80u) return 0xFFFD;
        cp = (cp << 6) | (u[k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    *used = need + 1;
    return cp;
}

void utf8_encode(Buf *out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        buf_putc(out, (char)cp);
    } else if (cp < 0x800) {
        buf_putc(out, (char)(0xC0u | (cp >> 6)));
        buf_putc(out, (char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        buf_putc(out, (char)(0xE0u | (cp >> 12)));
        buf_putc(out, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        buf_putc(out, (char)(0x80u | (cp & 0x3Fu)));
    } else {
        buf_putc(out, (char)(0xF0u | (cp >> 18)));
        buf_putc(out, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
        buf_putc(out, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        buf_putc(out, (char)(0x80u | (cp & 0x3Fu)));
    }
}

typedef struct {
    uint32_t lo, hi;
} Range;

static const Range ZERO_WIDTH[] = {
    {0x0300, 0x036F}, {0x200B, 0x200F}, {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
};

static const Range WIDE[] = {
    {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF},   {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x1F300, 0x1F64F},
    {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
};

static bool in_ranges(const Range *r, size_t n, uint32_t cp) {
    for (size_t i = 0; i < n; i++)
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    return false;
}

int cp_width(uint32_t cp) {
    if (in_ranges(ZERO_WIDTH, sizeof ZERO_WIDTH / sizeof ZERO_WIDTH[0], cp)) return 0;
    if (in_ranges(WIDE, sizeof WIDE / sizeof WIDE[0], cp)) return 2;
    return 1;
}

/* One display unit of the input. */
typedef struct {
    uint32_t cp; /* what to show (U+0020 for a tab) */
    size_t used; /* input bytes consumed */
    size_t w;    /* columns */
} Unit;

static Unit next_unit(const char *s, size_t n, size_t col) {
    Unit u;
    unsigned char c = (unsigned char)s[0];
    u.used = 1;
    if (c == '\t') {
        u.cp = ' ';
        u.w = TAB_WIDTH - (col % TAB_WIDTH);
    } else if (c < 0x20) {
        u.cp = 0x2400u + c; /* control picture, e.g. ESC -> U+241B */
        u.w = 1;
    } else if (c == 0x7F) {
        u.cp = 0x2421;
        u.w = 1;
    } else {
        u.cp = utf8_decode(s, n, &u.used);
        if (u.cp >= 0x80 && u.cp < 0xA0) u.cp = 0xFFFD; /* C1 controls */
        u.w = (size_t)cp_width(u.cp);
    }
    return u;
}

size_t text_width(const char *s, size_t n) {
    size_t col = 0, i = 0;
    while (i < n) {
        Unit u = next_unit(s + i, n - i, col);
        i += u.used;
        col += u.w;
    }
    return col;
}

size_t text_window(Buf *out, const char *s, size_t n, size_t skip, size_t cols) {
    size_t end_col = cols > SIZE_MAX - skip ? SIZE_MAX : skip + cols;
    size_t col = 0, emitted = 0, i = 0;

    while (i < n && col < end_col) {
        Unit u = next_unit(s + i, n - i, col);
        i += u.used;

        if (u.w == 0) { /* combining mark: keep it only after a visible character */
            if (col > skip) utf8_encode(out, u.cp);
            continue;
        }
        size_t next = col + u.w;
        if (next <= skip) { /* entirely left of the window */
            col = next;
            continue;
        }
        if (col < skip || next > end_col) { /* cut by an edge: show the visible part as blanks */
            size_t from = col < skip ? skip : col;
            size_t to = next > end_col ? end_col : next;
            for (size_t k = from; k < to; k++) buf_putc(out, ' ');
            emitted += to - from;
        } else if (s[i - u.used] == '\t') {
            for (size_t k = 0; k < u.w; k++) buf_putc(out, ' ');
            emitted += u.w;
        } else {
            utf8_encode(out, u.cp);
            emitted += u.w;
        }
        col = next;
    }
    return emitted;
}