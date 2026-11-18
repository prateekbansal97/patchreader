#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "text.h"

/* Inputs are copied into exact-size heap blocks so ASan flags any read past
 * the end of the given length. */
static char *heap(const char *s, size_t n) {
    char *p = xmalloc(n);
    if (n) memcpy(p, s, n);
    return p;
}

/* ---- utf8_decode ---- */

static uint32_t dec(const char *s, size_t n, size_t *used) {
    char *p = heap(s, n);
    uint32_t cp = utf8_decode(p, n, used);
    free(p);
    return cp;
}

#define DEC(lit, expect_cp, expect_used)                  \
    do {                                                  \
        size_t u_ = 0;                                    \
        uint32_t c_ = dec(lit, sizeof(lit) - 1, &u_);     \
        CHECK(c_ == (uint32_t)(expect_cp));               \
        CHECK(u_ == (size_t)(expect_used));               \
    } while (0)

static void test_decode(void) {
    /* valid, including the edges of every length class */
    DEC("A", 0x41, 1);
    DEC("\0", 0, 1);
    DEC("\x7f", 0x7f, 1);
    DEC("\xC2\x80", 0x80, 2);
    DEC("\xC3\xA9", 0xE9, 2);
    DEC("\xDF\xBF", 0x7FF, 2);
    DEC("\xE0\xA0\x80", 0x800, 3);
    DEC("\xE4\xB8\xAD", 0x4E2D, 3);
    DEC("\xED\x9F\xBF", 0xD7FF, 3);
    DEC("\xEE\x80\x80", 0xE000, 3);
    DEC("\xEF\xBF\xBF", 0xFFFF, 3);
    DEC("\xF0\x90\x80\x80", 0x10000, 4);
    DEC("\xF0\x9F\x98\x80", 0x1F600, 4);
    DEC("\xF4\x8F\xBF\xBF", 0x10FFFF, 4);

    /* invalid: always U+FFFD and exactly one byte consumed */
    DEC("\x80", 0xFFFD, 1);             /* lone continuation */
    DEC("\xBF", 0xFFFD, 1);
    DEC("\xC0\x80", 0xFFFD, 1);         /* overlong */
    DEC("\xC1\xBF", 0xFFFD, 1);
    DEC("\xE0\x80\x80", 0xFFFD, 1);
    DEC("\xE0\x9F\xBF", 0xFFFD, 1);
    DEC("\xF0\x80\x80\x80", 0xFFFD, 1);
    DEC("\xED\xA0\x80", 0xFFFD, 1);     /* surrogate */
    DEC("\xED\xBF\xBF", 0xFFFD, 1);
    DEC("\xF4\x90\x80\x80", 0xFFFD, 1); /* above U+10FFFF */
    DEC("\xF5\x80\x80\x80", 0xFFFD, 1);
    DEC("\xF8\x88\x80\x80\x80", 0xFFFD, 1);
    DEC("\xFE", 0xFFFD, 1);
    DEC("\xFF", 0xFFFD, 1);
    DEC("\xC3\x28", 0xFFFD, 1);         /* bad continuation */
    DEC("\xE4\xB8\x28", 0xFFFD, 1);
    DEC("\xC3", 0xFFFD, 1);             /* truncated by the length */
    DEC("\xE4", 0xFFFD, 1);
    DEC("\xE4\xB8", 0xFFFD, 1);
    DEC("\xF0\x9F\x98", 0xFFFD, 1);
}

static void test_decode_random(void) {
    int bad = 0;
    for (int it = 0; it < 100000; it++) {
        size_t n = 1 + rnd() % 6;
        char tmp[6];
        for (size_t k = 0; k < n; k++) tmp[k] = (char)(rnd() & 0xff);
        char *p = heap(tmp, n);
        size_t used = 0;
        uint32_t cp = utf8_decode(p, n, &used);
        if (used < 1 || used > n) bad++;
        unsigned char first = (unsigned char)p[0];
        if (used == 1 && first < 0x80 && cp != first) bad++;
        if (used == 1 && first >= 0x80 && cp != 0xFFFD) bad++;
        if (used > 1) { /* accepted multi-byte sequences must be canonical */
            Buf b;
            buf_init(&b);
            utf8_encode(&b, cp);
            if (b.len != used || memcmp(b.data, p, used) != 0) bad++;
            buf_free(&b);
        }
        free(p);
    }
    CHECK(bad == 0);
}

/* ---- utf8_encode ---- */

static void test_encode(void) {
    Buf b;
    buf_init(&b);
    utf8_encode(&b, 'A');
    CHECK(b.len == 1 && b.data[0] == 'A');
    buf_clear(&b);
    utf8_encode(&b, 0xE9);
    CHECK(b.len == 2 && memcmp(b.data, "\xC3\xA9", 2) == 0);
    buf_clear(&b);
    utf8_encode(&b, 0x4E2D);
    CHECK(b.len == 3 && memcmp(b.data, "\xE4\xB8\xAD", 3) == 0);
    buf_clear(&b);
    utf8_encode(&b, 0x1F600);
    CHECK(b.len == 4 && memcmp(b.data, "\xF0\x9F\x98\x80", 4) == 0);

    uint32_t bad_cps[] = {0xD800, 0xDFFF, 0x110000, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof bad_cps / sizeof bad_cps[0]; i++) {
        buf_clear(&b);
        utf8_encode(&b, bad_cps[i]);
        CHECK(b.len == 3 && memcmp(b.data, "\xEF\xBF\xBD", 3) == 0);
    }

    /* every valid code point survives encode -> decode */
    int bad = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        buf_clear(&b);
        utf8_encode(&b, cp);
        size_t used = 0;
        uint32_t back = utf8_decode(b.data, b.len, &used);
        if (back != cp || used != b.len) bad++;
    }
    CHECK(bad == 0);
    buf_free(&b);
}

/* ---- widths ---- */

static void test_cp_width(void) {
    CHECK(cp_width('a') == 1);
    CHECK(cp_width(0xE9) == 1);
    CHECK(cp_width(0x4E2D) == 2);  /* CJK */
    CHECK(cp_width(0x3042) == 2);  /* hiragana */
    CHECK(cp_width(0xAC00) == 2);  /* hangul */
    CHECK(cp_width(0xFF21) == 2);  /* fullwidth A */
    CHECK(cp_width(0x1F600) == 2); /* emoji */
    CHECK(cp_width(0x0301) == 0);  /* combining acute */
    CHECK(cp_width(0x200B) == 0);  /* zero-width space */
    CHECK(cp_width(0xFEFF) == 0);  /* BOM */
    CHECK(cp_width(0xFFFD) == 1);
    CHECK(cp_width(0x2400) == 1);
}

#define TW(lit, expect) CHECK(text_width(lit, sizeof(lit) - 1) == (size_t)(expect))

static void test_text_width(void) {
    CHECK(text_width("", 0) == 0);
    TW("abc", 3);
    TW("\t", 4);
    TW("a\t", 4);
    TW("abcd\t", 8);
    TW("\t\t", 8);
    TW("a\tb", 5);
    TW("\xE4\xB8\xAD\xE6\x96\x87", 4);
    TW("e\xCC\x81", 1);
    TW("\x1b", 1);
    TW("\x7f", 1);
    TW("\xff", 1);
    TW("\xC2\x9B", 1); /* C1 control shown as one replacement character */
}

/* ---- text_window ---- */

static void win_eq(const char *in, size_t n, size_t skip, size_t cols, const char *expect,
                   size_t expect_cols, int line) {
    char *p = heap(in, n);
    Buf b;
    buf_init(&b);
    size_t got = text_window(&b, p, n, skip, cols);
    size_t el = strlen(expect);
    int ok = got == expect_cols && b.len == el && memcmp(b.data, expect, el) == 0;
    CHECK(ok);
    if (!ok) fprintf(stderr, "    (window case at line %d: got %zu cols, %zu bytes)\n", line, got, b.len);
    buf_free(&b);
    free(p);
}

#define WIN(lit, skip, cols, expect, ecols) \
    win_eq(lit, sizeof(lit) - 1, (skip), (cols), (expect), (ecols), __LINE__)

static void test_window_basic(void) {
    WIN("hello", 0, 10, "hello", 5);
    WIN("hello", 1, 3, "ell", 3);
    WIN("hello", 3, 10, "lo", 2);
    WIN("hello", 10, 5, "", 0);
    WIN("hello", 5, 5, "", 0);
    WIN("hello", 0, 0, "", 0);
    WIN("", 0, 10, "", 0);
    WIN("abc", 0, SIZE_MAX, "abc", 3);
    WIN("abc", SIZE_MAX, SIZE_MAX, "", 0);
    WIN("abc", 1, SIZE_MAX, "bc", 2);
}

static void test_window_tabs(void) {
    WIN("a\tb", 0, 99, "a   b", 5);
    WIN("a\tb", 2, 2, "  ", 2); /* window inside the tab */
    WIN("a\tb", 3, 5, " b", 2); /* window starts inside the tab */
    WIN("\t", 0, 2, "  ", 2);   /* window ends inside the tab */
}

static void test_window_control_and_invalid(void) {
    /* ESC and friends can never reach the terminal as themselves */
    WIN("\x1b[31m", 0, 99, "\xE2\x90\x9B[31m", 5);
    WIN("\x7f", 0, 9, "\xE2\x90\xA1", 1);
    WIN("\n", 0, 9, "\xE2\x90\x8A", 1);
    WIN("\0", 0, 9, "\xE2\x90\x80", 1);
    /* CSI encoded as a UTF-8 C1 control is neutralised too */
    WIN("\xC2\x9B", 0, 9, "\xEF\xBF\xBD", 1);
    /* invalid and truncated UTF-8 */
    WIN("a\xFF" "b", 0, 99, "a\xEF\xBF\xBD" "b", 3);
    WIN("ab\xE4\xB8", 0, 9, "ab\xEF\xBF\xBD\xEF\xBF\xBD", 4);
}

static void test_window_wide_and_combining(void) {
    /* "中文x": columns 0-1 中, 2-3 文, 4 x */
    WIN("\xE4\xB8\xAD\xE6\x96\x87x", 0, 10, "\xE4\xB8\xAD\xE6\x96\x87x", 5);
    WIN("\xE4\xB8\xAD\xE6\x96\x87x", 1, 4, " \xE6\x96\x87x", 4); /* left edge cuts 中 */
    WIN("\xE4\xB8\xAD\xE6\x96\x87x", 0, 3, "\xE4\xB8\xAD ", 3);   /* right edge cuts 文 */
    WIN("\xE4\xB8\xAD\xE6\x96\x87x", 1, 1, " ", 1);
    WIN("\xE4\xB8\xAD\xE6\x96\x87x", 2, 2, "\xE6\x96\x87", 2);
    /* combining mark stays with its base, is dropped when the base is off-screen */
    WIN("e\xCC\x81x", 0, 2, "e\xCC\x81x", 2);
    WIN("e\xCC\x81x", 1, 5, "x", 1);
}

/* ---- property test ---- */

typedef struct {
    const char *s;
    size_t n;
} Frag;
#define FRAG(lit) {lit, sizeof(lit) - 1}

static const Frag FRAGS[] = {
    FRAG("a"),          FRAG("hello"),          FRAG(" "),        FRAG("\t"),
    FRAG("\x1b"),       FRAG("\x7f"),           FRAG("\n"),       FRAG("\r"),
    FRAG("\0"),         FRAG("\xC3\xA9"),       FRAG("\xE4\xB8\xAD"), FRAG("\xF0\x9F\x98\x80"),
    FRAG("\xCC\x81"),   FRAG("\xFF"),           FRAG("\xC2\x9B"), FRAG("\xE4\xB8"),
    FRAG("\xE4"),       FRAG("\xEF\xBB\xBF"),   FRAG("\xE2\x80\x8B"), FRAG("\xF0\x9F"),
};

/* Output is terminal-safe: no raw control bytes, only well-formed UTF-8. */
static int clean(const Buf *b) {
    size_t i = 0;
    while (i < b->len) {
        unsigned char c = (unsigned char)b->data[i];
        if (c < 0x20 || c == 0x7f) return 0;
        size_t used = 0;
        uint32_t cp = utf8_decode(b->data + i, b->len - i, &used);
        if (cp == 0xFFFD && used == 1) return 0; /* malformed byte left in the output */
        if (cp >= 0x80 && cp < 0xA0) return 0;
        i += used;
    }
    return 1;
}

static void test_window_properties(void) {
    int bad = 0;
    for (int it = 0; it < 20000; it++) {
        Buf in;
        buf_init(&in);
        uint32_t parts = rnd() % 40;
        for (uint32_t k = 0; k < parts; k++) {
            if (rnd() % 8 == 0) {
                buf_putc(&in, (char)(rnd() & 0xff));
            } else {
                const Frag *f = &FRAGS[rnd() % (sizeof FRAGS / sizeof FRAGS[0])];
                buf_append(&in, f->s, f->n);
            }
        }
        char *p = heap(in.data, in.len);
        size_t total = text_width(p, in.len);

        Buf all;
        buf_init(&all);
        size_t e_all = text_window(&all, p, in.len, 0, SIZE_MAX);
        if (e_all != total || !clean(&all) || text_width(all.data, all.len) != e_all) bad++;

        /* splitting the window at any column loses and duplicates nothing */
        size_t k = total ? rnd() % (total + 1) : 0;
        Buf l, r;
        buf_init(&l);
        buf_init(&r);
        size_t e1 = text_window(&l, p, in.len, 0, k);
        size_t e2 = text_window(&r, p, in.len, k, SIZE_MAX);
        if (e1 != k || e1 + e2 != total || !clean(&l) || !clean(&r)) bad++;

        /* an arbitrary window never exceeds its width and stays well-formed */
        Buf w;
        buf_init(&w);
        size_t skip = rnd() % (total + 3), cols = rnd() % 12;
        size_t e3 = text_window(&w, p, in.len, skip, cols);
        if (e3 > cols || !clean(&w) || text_width(w.data, w.len) != e3) bad++;

        buf_free(&w);
        buf_free(&l);
        buf_free(&r);
        buf_free(&all);
        free(p);
        buf_free(&in);
    }
    CHECK(bad == 0);
}

int main(void) {
    printf("test_text\n");
    test_decode();
    test_decode_random();
    test_encode();
    test_cp_width();
    test_text_width();
    test_window_basic();
    test_window_tabs();
    test_window_control_and_invalid();
    test_window_wide_and_combining();
    test_window_properties();
    TEST_DONE();
}
