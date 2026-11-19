#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "input.h"

/* Exact-size heap copy so ASan flags any read past n. */
static unsigned char *heap(const char *s, size_t n) {
    unsigned char *p = xmalloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

static size_t parse(const char *s, size_t n, Key *k) {
    unsigned char *p = heap(s, n);
    size_t used = key_parse(p, n, k);
    free(p);
    return used;
}

#define KEYIS(lit, t, c, u)                                         \
    do {                                                            \
        Key k_;                                                     \
        size_t used_ = parse(lit, sizeof(lit) - 1, &k_);            \
        CHECK(used_ == (u));                                        \
        CHECK(k_.type == (t));                                      \
        CHECK(k_.ch == (uint32_t)(c));                              \
    } while (0)

#define INCOMPLETE(lit)                                             \
    do {                                                            \
        Key k_;                                                     \
        CHECK(parse(lit, sizeof(lit) - 1, &k_) == 0);               \
    } while (0)

static void test_basic(void) {
    KEYIS("q", KEY_CHAR, 'q', 1);
    KEYIS("Z", KEY_CHAR, 'Z', 1);
    KEYIS(" ", KEY_CHAR, ' ', 1);
    KEYIS("/", KEY_CHAR, '/', 1);
    KEYIS("\r", KEY_ENTER, 0, 1);
    KEYIS("\n", KEY_ENTER, 0, 1);
    KEYIS("\t", KEY_TAB, 0, 1);
    KEYIS("\x7f", KEY_BACKSPACE, 0, 1);
    KEYIS("\x08", KEY_BACKSPACE, 0, 1);
    KEYIS("\x04", KEY_CTRL, 'd', 1);
    KEYIS("\x15", KEY_CTRL, 'u', 1);
    KEYIS("\x01", KEY_CTRL, 'a', 1);
    KEYIS("qrest", KEY_CHAR, 'q', 1); /* only the first key is consumed */
}

static void test_escapes(void) {
    KEYIS("\x1b[A", KEY_UP, 0, 3);
    KEYIS("\x1b[B", KEY_DOWN, 0, 3);
    KEYIS("\x1b[C", KEY_RIGHT, 0, 3);
    KEYIS("\x1b[D", KEY_LEFT, 0, 3);
    KEYIS("\x1bOA", KEY_UP, 0, 3);
    KEYIS("\x1bOB", KEY_DOWN, 0, 3);
    KEYIS("\x1bOC", KEY_RIGHT, 0, 3);
    KEYIS("\x1bOD", KEY_LEFT, 0, 3);
    KEYIS("\x1b[H", KEY_HOME, 0, 3);
    KEYIS("\x1b[F", KEY_END, 0, 3);
    KEYIS("\x1bOH", KEY_HOME, 0, 3);
    KEYIS("\x1bOF", KEY_END, 0, 3);
    KEYIS("\x1b[1~", KEY_HOME, 0, 4);
    KEYIS("\x1b[7~", KEY_HOME, 0, 4);
    KEYIS("\x1b[4~", KEY_END, 0, 4);
    KEYIS("\x1b[8~", KEY_END, 0, 4);
    KEYIS("\x1b[3~", KEY_DEL, 0, 4);
    KEYIS("\x1b[5~", KEY_PGUP, 0, 4);
    KEYIS("\x1b[6~", KEY_PGDN, 0, 4);
    KEYIS("\x1b[Z", KEY_BTAB, 0, 3);
    /* modifiers are ignored */
    KEYIS("\x1b[1;5C", KEY_RIGHT, 0, 6);
    KEYIS("\x1b[1;2A", KEY_UP, 0, 6);
    KEYIS("\x1b[5;3~", KEY_PGUP, 0, 6);
    /* trailing bytes are left alone */
    KEYIS("\x1b[Aq", KEY_UP, 0, 3);
    /* Alt-x: ESC reported alone, x follows */
    KEYIS("\x1bx", KEY_ESC, 0, 1);
    KEYIS("\x1b\x1b", KEY_ESC, 0, 1);
}

static void test_unknown(void) {
    KEYIS("\x1b[999~", KEY_NONE, 0, 6);
    KEYIS("\x1b[999;99~", KEY_NONE, 0, 9);
    KEYIS("\x1b[?25h", KEY_NONE, 0, 6);
    KEYIS("\x1bOQ", KEY_NONE, 0, 3);
    KEYIS("\x1b[200~", KEY_NONE, 0, 6);
    KEYIS("\x80", KEY_NONE, 0, 1);  /* stray continuation byte */
    KEYIS("\xff", KEY_NONE, 0, 1);
    KEYIS("\xc0\x80", KEY_NONE, 0, 1); /* overlong lead */

    /* a never-ending parameter run is cut off, not buffered forever */
    char junk[64];
    junk[0] = 0x1b;
    junk[1] = '[';
    memset(junk + 2, '1', sizeof junk - 2);
    Key k;
    size_t used = parse(junk, sizeof junk, &k);
    CHECK(used > 0);
    CHECK(used <= sizeof junk);
    CHECK(k.type == KEY_NONE);
}

static void test_incomplete(void) {
    Key k;
    CHECK(key_parse((const unsigned char *)"", 0, &k) == 0);
    INCOMPLETE("\x1b");
    INCOMPLETE("\x1b[");
    INCOMPLETE("\x1b[1");
    INCOMPLETE("\x1b[1;");
    INCOMPLETE("\x1b[1;5");
    INCOMPLETE("\x1bO");
    INCOMPLETE("\xc3");         /* half of U+00E9 */
    INCOMPLETE("\xe2\x82");     /* two thirds of U+20AC */
    INCOMPLETE("\xf0\x9f\x98"); /* three quarters of an emoji */
}

static void test_utf8(void) {
    KEYIS("\xc3\xa9", KEY_CHAR, 0xE9, 2);
    KEYIS("\xe2\x82\xac", KEY_CHAR, 0x20AC, 3);
    KEYIS("\xf0\x9f\x98\x80", KEY_CHAR, 0x1F600, 4);
    KEYIS("\xc3\xa9x", KEY_CHAR, 0xE9, 2);
    /* a lead byte followed by a non-continuation is junk, then 'x' is a key */
    KEYIS("\xc3x", KEY_NONE, 0, 1);
}

static void test_describe(void) {
    char buf[32];
    Key k;

    k.type = KEY_UP; k.ch = 0;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "Up") == 0);
    k.type = KEY_PGDN;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "PageDown") == 0);
    k.type = KEY_BTAB;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "Shift-Tab") == 0);
    k.type = KEY_ESC;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "Esc") == 0);
    k.type = KEY_CHAR; k.ch = 'q';
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "'q'") == 0);
    k.ch = 0xE9;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "U+00E9") == 0);
    k.ch = 0x1F600;
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "U+1F600") == 0);
    k.type = KEY_CTRL; k.ch = 'd';
    key_describe(k, buf, sizeof buf);
    CHECK(strcmp(buf, "Ctrl-d") == 0);

    /* a tiny buffer truncates but stays NUL-terminated */
    char small[4];
    memset(small, 'x', sizeof small);
    k.type = KEY_PGDN; k.ch = 0;
    key_describe(k, small, sizeof small);
    CHECK(memchr(small, 0, sizeof small) != NULL);
    CHECK(strcmp(small, "Pag") == 0);
}

/* Random bytes: never reads out of bounds, never consumes more than given,
 * and a nonzero result always makes progress. Feeding a whole stream key by
 * key always terminates. */
static void test_random(void) {
    for (int iter = 0; iter < 20000; iter++) {
        size_t n = rnd() % 40;
        unsigned char *p = xmalloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = rnd();
            switch (r % 6) {
            case 0: p[i] = 0x1b; break;
            case 1: p[i] = (unsigned char)"[O;~0123456789ABCDFHZ"[(r >> 8) % 21]; break;
            default: p[i] = (unsigned char)(r >> 8); break;
            }
        }
        size_t off = 0;
        int guard = 0;
        while (off < n) {
            Key k;
            size_t used = key_parse(p + off, n - off, &k);
            if (used == 0) break; /* incomplete tail */
            CHECK(used <= n - off);
            off += used;
            if (++guard > 100) { CHECK(0); break; }
        }
        free(p);
    }
}

int main(void) {
    printf("test_input\n");
    test_basic();
    test_escapes();
    test_unknown();
    test_incomplete();
    test_utf8();
    test_describe();
    test_random();
    TEST_DONE();
}
