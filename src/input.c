#include "input.h"

#include <stdbool.h>
#include <stdio.h>

#include "text.h"

#define MAX_CSI_LEN 32

static size_t set(Key *out, KeyType t, size_t used) {
    out->type = t;
    return used;
}

/* ESC [ params intermediates final  |  ESC O final  |  ESC (alone or Alt-x) */
static size_t parse_escape(const unsigned char *in, size_t n, Key *out) {
    if (n < 2) return 0;

    if (in[1] == 'O') {
        if (n < 3) return 0;
        switch (in[2]) {
        case 'A': return set(out, KEY_UP, 3);
        case 'B': return set(out, KEY_DOWN, 3);
        case 'C': return set(out, KEY_RIGHT, 3);
        case 'D': return set(out, KEY_LEFT, 3);
        case 'H': return set(out, KEY_HOME, 3);
        case 'F': return set(out, KEY_END, 3);
        default: return set(out, KEY_NONE, 3);
        }
    }

    if (in[1] != '[') return set(out, KEY_ESC, 1); /* Alt-x: report the ESC, x follows */

    size_t i = 2;
    unsigned first = 0;
    bool in_first = true;
    while (i < n && in[i] >= 0x30 && in[i] <= 0x3F) { /* parameter bytes */
        if (in_first) {
            if (in[i] >= '0' && in[i] <= '9') {
                if (first < 10000) first = first * 10 + (unsigned)(in[i] - '0');
            } else {
                in_first = false; /* ';' ':' or a private marker ends the first number */
            }
        }
        i++;
        if (i > MAX_CSI_LEN) return set(out, KEY_NONE, i);
    }
    while (i < n && in[i] >= 0x20 && in[i] <= 0x2F) { /* intermediate bytes */
        i++;
        if (i > MAX_CSI_LEN) return set(out, KEY_NONE, i);
    }
    if (i >= n) return 0;
    unsigned char f = in[i];
    size_t used = i + 1;
    if (f < 0x40 || f > 0x7E) return set(out, KEY_NONE, used);

    switch (f) {
    case 'A': return set(out, KEY_UP, used);
    case 'B': return set(out, KEY_DOWN, used);
    case 'C': return set(out, KEY_RIGHT, used);
    case 'D': return set(out, KEY_LEFT, used);
    case 'H': return set(out, KEY_HOME, used);
    case 'F': return set(out, KEY_END, used);
    case 'Z': return set(out, KEY_BTAB, used);
    case '~':
        switch (first) {
        case 1:
        case 7: return set(out, KEY_HOME, used);
        case 3: return set(out, KEY_DEL, used);
        case 4:
        case 8: return set(out, KEY_END, used);
        case 5: return set(out, KEY_PGUP, used);
        case 6: return set(out, KEY_PGDN, used);
        default: break;
        }
        break;
    default: break;
    }
    return set(out, KEY_NONE, used);
}

static size_t utf8_expected(unsigned char lead) {
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 1;
}

size_t key_parse(const unsigned char *in, size_t n, Key *out) {
    out->type = KEY_NONE;
    out->ch = 0;
    if (n == 0) return 0;

    unsigned char c = in[0];
    if (c == 0x1B) return parse_escape(in, n, out);
    if (c == '\r' || c == '\n') return set(out, KEY_ENTER, 1);
    if (c == '\t') return set(out, KEY_TAB, 1);
    if (c == 0x7F || c == 0x08) return set(out, KEY_BACKSPACE, 1);
    if (c < 0x20) {
        out->ch = c <= 0x1A ? (uint32_t)('a' + c - 1) : (uint32_t)(c + 0x40);
        return set(out, KEY_CTRL, 1);
    }

    size_t want = utf8_expected(c);
    if (n < want) { /* a split multi-byte character: wait for the rest */
        bool tail_ok = true;
        for (size_t k = 1; k < n; k++)
            if ((in[k] & 0xC0u) != 0x80u) tail_ok = false;
        if (tail_ok) return 0;
    }
    size_t used;
    uint32_t cp = utf8_decode((const char *)in, n, &used);
    if (cp == 0xFFFD && used == 1) return set(out, KEY_NONE, 1);
    out->ch = cp;
    return set(out, KEY_CHAR, used);
}

void key_describe(Key k, char *out, size_t n) {
    const char *name = NULL;
    switch (k.type) {
    case KEY_UP: name = "Up"; break;
    case KEY_DOWN: name = "Down"; break;
    case KEY_LEFT: name = "Left"; break;
    case KEY_RIGHT: name = "Right"; break;
    case KEY_PGUP: name = "PageUp"; break;
    case KEY_PGDN: name = "PageDown"; break;
    case KEY_HOME: name = "Home"; break;
    case KEY_END: name = "End"; break;
    case KEY_DEL: name = "Delete"; break;
    case KEY_TAB: name = "Tab"; break;
    case KEY_BTAB: name = "Shift-Tab"; break;
    case KEY_ENTER: name = "Enter"; break;
    case KEY_BACKSPACE: name = "Backspace"; break;
    case KEY_ESC: name = "Esc"; break;
    case KEY_RESIZE: name = "resize"; break;
    case KEY_EOF: name = "EOF"; break;
    case KEY_NONE: name = "unknown"; break;
    case KEY_CHAR:
        if (k.ch >= 0x20 && k.ch < 0x7F) snprintf(out, n, "'%c'", (char)k.ch);
        else snprintf(out, n, "U+%04X", (unsigned)k.ch);
        return;
    case KEY_CTRL:
        snprintf(out, n, "Ctrl-%c", (char)k.ch);
        return;
    }
    snprintf(out, n, "%s", name);
}