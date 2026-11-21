#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "model.h"
#include "nav.h"
#include "patch.h"
#include "render.h"
#include "search.h"
#include "text.h"
#include "util.h"
#include "worddiff.h"

#define REQUIRE(cond)     \
    do {                  \
        int ok_ = (cond); \
        CHECK(ok_);       \
        if (!ok_) return; \
    } while (0)

static char *heap(const char *s, size_t n) {
    char *p = xmalloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

/* ---- search.c ---- */

static size_t find(const char *s, const char *q, bool ic, size_t from) {
    size_t n = strlen(s), qn = strlen(q);
    char *hs = heap(s, n), *hq = heap(q, qn);
    size_t r = search_find(hs, n, hq, qn, ic, from);
    free(hs);
    free(hq);
    return r;
}

static void test_search_find(void) {
    CHECK(search_icase("abc", 3));
    CHECK(!search_icase("aBc", 3));
    CHECK(search_icase("", 0));
    CHECK(search_icase("a1_ \xc3\xa9", 6));

    CHECK(find("hello world", "world", false, 0) == 6);
    CHECK(find("hello world", "hello", false, 0) == 0);
    CHECK(find("hello world", "d", false, 0) == 10);
    CHECK(find("hello world", "o", false, 0) == 4);
    CHECK(find("hello world", "o", false, 5) == 7);
    CHECK(find("hello world", "o", false, 8) == SIZE_MAX);
    CHECK(find("hello world", "xyz", false, 0) == SIZE_MAX);
    CHECK(find("hello world", "hello world", false, 0) == 0);
    CHECK(find("hello world", "hello world!", false, 0) == SIZE_MAX);
    CHECK(find("hello", "", false, 0) == SIZE_MAX);
    CHECK(find("", "a", false, 0) == SIZE_MAX);
    CHECK(find("", "", false, 0) == SIZE_MAX);
    CHECK(find("abc", "c", false, 3) == SIZE_MAX); /* from == n */
    CHECK(find("abc", "c", false, 99) == SIZE_MAX);
    CHECK(find("abc", "c", false, 2) == 2);
    CHECK(find("aaa", "aa", false, 0) == 0);
    CHECK(find("aaa", "aa", false, 1) == 1);
    CHECK(find("aaa", "aa", false, 2) == SIZE_MAX);
    CHECK(find("abcabd", "abd", false, 0) == 3); /* partial match first */

    /* case */
    CHECK(find("Hello", "hello", true, 0) == 0);
    CHECK(find("Hello", "hello", false, 0) == SIZE_MAX);
    CHECK(find("Hello", "Hello", false, 0) == 0);
    CHECK(find("xHELLOx", "hello", true, 0) == 1);
    CHECK(find("ABC", "abd", true, 0) == SIZE_MAX);
    /* ASCII only: non-ASCII bytes are compared exactly */
    CHECK(find("caf\xc3\xa9", "\xc3\xa9", true, 0) == 3);
    CHECK(find("caf\xc3\x89", "\xc3\xa9", true, 0) == SIZE_MAX);
    /* NUL-free but with control and high bytes */
    CHECK(find("a\x01\xff" "b", "\x01\xff", false, 0) == 1);
}

static void spans_of(const char *s, const char *q, bool ic, Vec *v) {
    search_spans(s, strlen(s), q, strlen(q), ic, v);
}

static void test_search_spans(void) {
    Vec sp;
    vec_init(&sp, sizeof(Span));
    spans_of("one two one", "one", true, &sp);
    REQUIRE(sp.len == 2);
    CHECK(((Span *)vec_at(&sp, 0))->start == 0 && ((Span *)vec_at(&sp, 0))->end == 3);
    CHECK(((Span *)vec_at(&sp, 1))->start == 8 && ((Span *)vec_at(&sp, 1))->end == 11);
    vec_free(&sp);

    /* matches never overlap */
    vec_init(&sp, sizeof(Span));
    spans_of("aaaaa", "aa", false, &sp);
    REQUIRE(sp.len == 2);
    CHECK(((Span *)vec_at(&sp, 1))->start == 2 && ((Span *)vec_at(&sp, 1))->end == 4);
    vec_free(&sp);

    vec_init(&sp, sizeof(Span));
    spans_of("abc", "", true, &sp);
    CHECK(sp.len == 0);
    spans_of("abc", "zz", true, &sp);
    CHECK(sp.len == 0);
    spans_of("ABC abc", "abc", true, &sp);
    CHECK(sp.len == 2);
    vec_free(&sp);

    /* a row full of matches is capped */
    char *big = xmalloc(1000);
    memset(big, 'a', 1000);
    vec_init(&sp, sizeof(Span));
    search_spans(big, 1000, "a", 1, false, &sp);
    CHECK(sp.len == SEARCH_MAX_SPANS);
    CHECK(((Span *)vec_at(&sp, SEARCH_MAX_SPANS - 1))->end == SEARCH_MAX_SPANS);
    vec_free(&sp);
    free(big);
}

static size_t naive(const char *s, size_t n, const char *q, size_t qn, bool ic, size_t from) {
    if (qn == 0) return SIZE_MAX;
    for (size_t i = from; i + qn <= n; i++) {
        size_t k = 0;
        for (; k < qn; k++) {
            unsigned char a = (unsigned char)s[i + k], b = (unsigned char)q[k];
            if (ic) {
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
            }
            if (a != b) break;
        }
        if (k == qn) return i;
    }
    return SIZE_MAX;
}

static void test_search_random(void) {
    static const char alpha[] = "aAbB \xc3\xa9\x01";
    for (int iter = 0; iter < 30000; iter++) {
        char s[40], q[6];
        size_t n = rnd() % 40, qn = rnd() % 6;
        for (size_t i = 0; i < n; i++) s[i] = alpha[rnd() % (sizeof alpha - 1)];
        for (size_t i = 0; i < qn; i++) q[i] = alpha[rnd() % (sizeof alpha - 1)];
        bool ic = rnd() & 1;
        size_t from = rnd() % (n + 2);
        char *hs = heap(s, n), *hq = heap(q, qn);
        size_t got = search_find(hs, n, hq, qn, ic, from);
        size_t want = from > n ? SIZE_MAX : naive(hs, n, hq, qn, ic, from);
        CHECK(got == want);
        free(hs);
        free(hq);
    }
}

/* ---- navigation: search, file list, help ---- */

static void make_patch(Buf *b) {
    buf_clear(b);
    buf_puts(b,
             "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n"
             "@@ -1,3 +1,3 @@ int main\n"
             " alpha one\n-Beta two\n+beta three\n gamma one\n"
             "@@ -20,2 +20,2 @@\n"
             " one more\n-del\n+add one\n"
             "diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-x\n+y\n"
             "diff --git a/c.c b/c.c\n--- a/c.c\n+++ b/c.c\n@@ -0,0 +1 @@\n+one\n");
}
/* a.c rows: 0 hunk, 1 "alpha one", 2 "Beta two", 3 "beta three", 4 "gamma one",
 *           5 hunk, 6 "one more", 7 "del", 8 "add one" */

typedef struct {
    Buf src;
    Patch p;
    Model m;
    Nav nav;
    int built;
} Fx;

static void fx_load(Fx *x, size_t file) {
    if (x->built) model_free(&x->m);
    model_build(&x->m, &x->p, vec_at(&x->p.files, file));
    x->built = 1;
}

static int fx_open(Fx *x) {
    ParseError err;
    buf_init(&x->src);
    make_patch(&x->src);
    x->built = 0;
    if (patch_parse(&x->p, x->src.data, x->src.len, &err) != 0) {
        patch_free(&x->p);
        buf_free(&x->src);
        return 0;
    }
    nav_init(&x->nav, x->p.files.len);
    fx_load(x, 0);
    return 1;
}

static void fx_close(Fx *x) {
    if (x->built) model_free(&x->m);
    patch_free(&x->p);
    buf_free(&x->src);
}

static Key K(KeyType t, uint32_t ch) {
    Key k;
    k.type = t;
    k.ch = ch;
    return k;
}
static Key ch(char c) { return K(KEY_CHAR, (uint32_t)(unsigned char)c); }

static bool press(Fx *x, Key k, int rows, int cols) {
    bool changed = nav_key(&x->nav, k, &x->m, rows, cols);
    if (changed) fx_load(x, x->nav.file);
    return changed;
}

static void type(Fx *x, const char *s) {
    for (; *s; s++) press(x, ch(*s), 24, 80);
}

/* "/" + text + Enter */
static void search(Fx *x, const char *text, int rows) {
    press(x, ch('/'), rows, 80);
    type(x, text);
    press(x, K(KEY_ENTER, 0), rows, 80);
}

static void test_nav_search(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;

    press(&x, ch('/'), 24, 80);
    CHECK(n->mode == NAV_SEARCH && n->input_len == 0);
    type(&x, "one");
    CHECK(n->input_len == 3 && strcmp(n->input, "one") == 0);
    CHECK(n->mode == NAV_SEARCH && !n->quit); /* letters are text, not commands */
    press(&x, K(KEY_ENTER, 0), 24, 80);
    CHECK(n->mode == NAV_VIEW);
    CHECK(strcmp(n->query, "one") == 0 && n->query_len == 3);
    CHECK(n->hit == 1);
    CHECK(strcmp(n->status, "/one  1/4") == 0);

    press(&x, ch('n'), 24, 80); CHECK(n->hit == 4 && strcmp(n->status, "/one  2/4") == 0);
    press(&x, ch('n'), 24, 80); CHECK(n->hit == 6 && strcmp(n->status, "/one  3/4") == 0);
    press(&x, ch('n'), 24, 80); CHECK(n->hit == 8 && strcmp(n->status, "/one  4/4") == 0);
    press(&x, ch('n'), 24, 80); CHECK(n->hit == 1 && strcmp(n->status, "/one  1/4  (wrapped)") == 0);
    press(&x, ch('N'), 24, 80); CHECK(n->hit == 8 && strcmp(n->status, "/one  4/4  (wrapped)") == 0);
    press(&x, ch('N'), 24, 80); CHECK(n->hit == 6 && strcmp(n->status, "/one  3/4") == 0);

    /* any other key clears the message but keeps the query */
    press(&x, ch('j'), 24, 80);
    CHECK(n->status[0] == '\0' && n->query_len == 3);
    press(&x, ch('n'), 24, 80);
    CHECK(n->hit == 8);

    /* the search is for the current file only; the query follows you */
    press(&x, ch(']'), 24, 80);
    CHECK(n->file == 1 && n->hit == SIZE_MAX && n->status[0] == '\0' && n->query_len == 3);
    press(&x, ch('n'), 24, 80);
    CHECK(strcmp(n->status, "Not found: one") == 0 && n->hit == SIZE_MAX);
    press(&x, ch(']'), 24, 80);
    press(&x, ch('n'), 24, 80); /* c.c has "+one" */
    CHECK(n->hit == 1 && strcmp(n->status, "/one  1/1") == 0);
    press(&x, ch('n'), 24, 80);
    CHECK(n->hit == 1 && strcmp(n->status, "/one  1/1  (wrapped)") == 0);
    fx_close(&x);
}

static void test_nav_search_case(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;

    search(&x, "Beta", 24);
    CHECK(n->hit == 2 && strcmp(n->status, "/Beta  1/1") == 0); /* capital: exact case */
    search(&x, "beta", 24);
    CHECK(n->hit == 2 && strcmp(n->status, "/beta  1/2") == 0); /* all lower: any case */
    press(&x, ch('n'), 24, 80);
    CHECK(n->hit == 3 && strcmp(n->status, "/beta  2/2") == 0);

    /* hunk rows are searchable too */
    search(&x, "int main", 24);
    CHECK(n->hit == 0);
    fx_close(&x);
}

static void test_nav_search_scroll(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;

    /* 5 screen rows: 3 body rows, so max_top is 6 and a third is 1 */
    search(&x, "add", 5);
    CHECK(n->hit == 8);
    CHECK(n->top == 6);

    n->top = 0;
    search(&x, "alpha", 5); /* row 1 is already visible: no scroll */
    CHECK(n->hit == 1 && n->top == 0);

    n->top = 5;
    search(&x, "gamma", 5); /* row 4 is above the view: scrolled into it */
    CHECK(n->hit == 4);
    CHECK(n->top == 3);

    /* a visible match does not move the view */
    n->top = 4; /* shows rows 4..6 */
    search(&x, "one more", 5);
    CHECK(n->hit == 6 && n->top == 4);
    fx_close(&x);
}

static void test_nav_search_edge(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;

    /* no query yet */
    press(&x, ch('n'), 24, 80);
    CHECK(strcmp(n->status, "No search pattern") == 0);
    press(&x, ch('N'), 24, 80);
    CHECK(strcmp(n->status, "No search pattern") == 0);
    search(&x, "", 24); /* Enter on an empty prompt with no old query */
    CHECK(strcmp(n->status, "No search pattern") == 0 && n->mode == NAV_VIEW);

    /* not found */
    n->top = 2;
    search(&x, "zzz", 24);
    CHECK(strcmp(n->status, "Not found: zzz") == 0);
    CHECK(n->hit == SIZE_MAX && n->top == 2);
    CHECK(strcmp(n->query, "zzz") == 0);
    press(&x, ch('n'), 24, 80);
    CHECK(strcmp(n->status, "Not found: zzz") == 0);

    /* Enter on an empty prompt repeats the old query from the last match */
    n->top = 0;
    search(&x, "one", 24);
    CHECK(n->hit == 1);
    search(&x, "", 24);
    CHECK(n->hit == 4 && strcmp(n->query, "one") == 0);

    /* Esc cancels and keeps the old query */
    press(&x, ch('/'), 24, 80);
    type(&x, "new");
    press(&x, K(KEY_ESC, 0), 24, 80);
    CHECK(n->mode == NAV_VIEW && strcmp(n->query, "one") == 0);

    /* typing again starts from an empty prompt */
    press(&x, ch('/'), 24, 80);
    CHECK(n->input_len == 0 && n->input[0] == '\0');
    fx_close(&x);
}

static void test_nav_search_input(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;

    press(&x, ch('/'), 24, 80);
    type(&x, "abc");
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    CHECK(strcmp(n->input, "ab") == 0);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    CHECK(n->input_len == 0 && n->mode == NAV_SEARCH);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80); /* backspace on an empty prompt leaves it */
    CHECK(n->mode == NAV_VIEW);

    /* Ctrl-u clears */
    press(&x, ch('/'), 24, 80);
    type(&x, "abc");
    press(&x, K(KEY_CTRL, 'u'), 24, 80);
    CHECK(n->input_len == 0 && n->input[0] == '\0' && n->mode == NAV_SEARCH);

    /* multi-byte characters are stored as UTF-8 and removed whole */
    press(&x, K(KEY_CHAR, 0xE9), 24, 80);
    press(&x, K(KEY_CHAR, 0x65E5), 24, 80);
    press(&x, K(KEY_CHAR, 0x1F600), 24, 80);
    CHECK(strcmp(n->input, "\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80") == 0);
    CHECK(n->input_len == 2 + 3 + 4);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    CHECK(n->input_len == 5);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    CHECK(n->input_len == 2 && strcmp(n->input, "\xc3\xa9") == 0);
    press(&x, K(KEY_BACKSPACE, 0), 24, 80);
    CHECK(n->input_len == 0);

    /* control characters and the like are not text */
    press(&x, K(KEY_CHAR, 0x07), 24, 80);
    press(&x, K(KEY_CHAR, 0x7F), 24, 80);
    press(&x, K(KEY_CHAR, 0x85), 24, 80);
    press(&x, K(KEY_CHAR, 0x1B), 24, 80);
    press(&x, K(KEY_TAB, 0), 24, 80);
    press(&x, K(KEY_UP, 0), 24, 80);
    CHECK(n->input_len == 0 && n->mode == NAV_SEARCH);

    /* the prompt is bounded and always NUL-terminated */
    for (int i = 0; i < 300; i++) press(&x, K(KEY_CHAR, i % 3 ? 'a' : 0xE9), 24, 80);
    CHECK(n->input_len < sizeof n->input);
    CHECK(n->input[n->input_len] == '\0');
    CHECK(strlen(n->input) == n->input_len);
    press(&x, K(KEY_ENTER, 0), 24, 80);
    CHECK(n->query_len == strlen(n->query));
    CHECK(n->mode == NAV_VIEW);

    /* closing the terminal while typing quits */
    press(&x, ch('/'), 24, 80);
    press(&x, K(KEY_EOF, 0), 24, 80);
    CHECK(n->quit);
    fx_close(&x);
}

/* ---- file list ---- */

static void test_nav_picker(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;
    const int R = 24, C = 80;

    n->top = 3;
    press(&x, ch('f'), R, C);
    CHECK(n->mode == NAV_PICK && n->pick == 0);

    press(&x, ch('j'), R, C); CHECK(n->pick == 1);
    press(&x, K(KEY_DOWN, 0), R, C); CHECK(n->pick == 2);
    press(&x, ch('j'), R, C); CHECK(n->pick == 2);        /* clamped */
    press(&x, ch('k'), R, C); CHECK(n->pick == 1);
    press(&x, K(KEY_UP, 0), R, C); CHECK(n->pick == 0);
    press(&x, K(KEY_UP, 0), R, C); CHECK(n->pick == 0);
    press(&x, ch('G'), R, C); CHECK(n->pick == 2);
    press(&x, ch('g'), R, C); CHECK(n->pick == 0);
    press(&x, K(KEY_END, 0), R, C); CHECK(n->pick == 2);
    press(&x, K(KEY_HOME, 0), R, C); CHECK(n->pick == 0);
    press(&x, K(KEY_PGDN, 0), R, C); CHECK(n->pick == 2);
    press(&x, K(KEY_PGUP, 0), R, C); CHECK(n->pick == 0);
    CHECK(n->top == 3 && n->file == 0 && n->mode == NAV_PICK); /* nothing else moved */

    /* Enter on a different file switches to it */
    press(&x, ch('j'), R, C);
    CHECK(press(&x, K(KEY_ENTER, 0), R, C) == true);
    CHECK(n->mode == NAV_VIEW && n->file == 1 && n->top == 0 && n->left == 0);
    CHECK(x.m.file == vec_at(&x.p.files, 1));

    /* the list opens on the current file; Enter there changes nothing */
    press(&x, ch('f'), R, C);
    CHECK(n->pick == 1);
    n->top = 1;
    CHECK(press(&x, K(KEY_ENTER, 0), R, C) == false);
    CHECK(n->mode == NAV_VIEW && n->file == 1 && n->top == 1);

    /* Esc, q and f close it without switching */
    const Key closers[] = {K(KEY_ESC, 0), ch('q'), ch('f')};
    for (size_t i = 0; i < 3; i++) {
        press(&x, ch('f'), R, C);
        press(&x, ch('j'), R, C);
        CHECK(press(&x, closers[i], R, C) == false);
        CHECK(n->mode == NAV_VIEW && n->file == 1 && !n->quit);
    }

    /* other commands do nothing inside the list */
    press(&x, ch('f'), R, C);
    press(&x, ch('/'), R, C);
    press(&x, ch('?'), R, C);
    press(&x, ch(']'), R, C);
    press(&x, ch('n'), R, C);
    press(&x, K(KEY_TAB, 0), R, C);
    CHECK(n->mode == NAV_PICK && n->file == 1);
    press(&x, K(KEY_EOF, 0), R, C);
    CHECK(n->quit);
    fx_close(&x);
}

static void test_nav_picker_scroll(void) {
    /* 40 files: the selection must stay inside the visible window */
    Buf b;
    buf_init(&b);
    for (int i = 0; i < 40; i++) {
        char s[96];
        snprintf(s, sizeof s, "--- a/f%d\n+++ b/f%d\n@@ -1 +1 @@\n-a\n+b\n", i, i);
        buf_puts(&b, s);
    }
    Patch p;
    ParseError err;
    REQUIRE(patch_parse(&p, b.data, b.len, &err) == 0);
    REQUIRE(p.files.len == 40);
    Model m;
    model_build(&m, &p, vec_at(&p.files, 0));
    Nav nv;
    nav_init(&nv, 40);
    const int R = 8; /* 6 visible entries */

    nav_key(&nv, ch('f'), &m, R, 40);
    for (int i = 0; i < 39; i++) {
        nav_key(&nv, ch('j'), &m, R, 40);
        CHECK(nv.pick == (size_t)i + 1);
        CHECK(nv.pick >= nv.pick_top && nv.pick - nv.pick_top < 6);
    }
    CHECK(nv.pick == 39 && nv.pick_top == 34);
    nav_key(&nv, ch('g'), &m, R, 40);
    CHECK(nv.pick == 0 && nv.pick_top == 0);
    nav_key(&nv, ch('G'), &m, R, 40);
    CHECK(nv.pick == 39 && nv.pick_top == 34);
    nav_key(&nv, K(KEY_PGUP, 0), &m, R, 40);
    CHECK(nv.pick == 34 && nv.pick - nv.pick_top < 6 && nv.pick >= nv.pick_top);

    /* a taller window pulls the list window back (nav_clamp as main does) */
    nav_clamp(&nv, &m, 50, 40);
    CHECK(nv.pick_top <= nv.pick && nv.pick - nv.pick_top < 48);
    nav_clamp(&nv, &m, 3, 40); /* 1 visible entry */
    CHECK(nv.pick_top == nv.pick);
    nav_clamp(&nv, &m, 1, 40); /* none */
    CHECK(nv.pick_top == nv.pick);

    /* random keys never leave the selection outside the list */
    for (int i = 0; i < 20000; i++) {
        static const KeyType t[] = {KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END,
                                    KEY_CHAR, KEY_ENTER, KEY_ESC, KEY_RESIZE, KEY_NONE};
        Key k = K(t[rnd() % 11], (uint32_t)(unsigned char)"jkgGbf /?n]q"[rnd() % 12]);
        int rows = (int)(1 + rnd() % 20);
        if (rnd() % 5 == 0) nv.mode = NAV_PICK;
        bool changed = nav_key(&nv, k, &m, rows, 40);
        nv.quit = false;
        if (changed) {
            model_free(&m);
            model_build(&m, &p, vec_at(&p.files, nv.file));
        }
        nav_clamp(&nv, &m, rows, 40);
        CHECK(nv.pick < 40 && nv.file < 40);
        CHECK(nv.pick >= nv.pick_top);
        size_t body = render_body_rows(rows);
        if (body > 0) CHECK(nv.pick - nv.pick_top < body);
    }
    model_free(&m);
    patch_free(&p);
    buf_free(&b);
}

/* ---- help ---- */

static void test_nav_help(void) {
    Fx x;
    REQUIRE(fx_open(&x));
    Nav *n = &x.nav;
    const int R = 24, C = 80;

    press(&x, ch('?'), R, C);
    CHECK(n->mode == NAV_HELP);
    press(&x, K(KEY_RESIZE, 0), R, C);
    press(&x, K(KEY_NONE, 0), R, C);
    CHECK(n->mode == NAV_HELP); /* a resize does not dismiss it */
    n->top = 0;
    press(&x, ch('j'), R, C);   /* the key only closes it */
    CHECK(n->mode == NAV_VIEW && n->top == 0);

    press(&x, ch('?'), R, C);
    press(&x, ch('q'), R, C);   /* q closes, it does not quit */
    CHECK(n->mode == NAV_VIEW && !n->quit);
    press(&x, ch('?'), R, C);
    press(&x, K(KEY_ESC, 0), R, C);
    CHECK(n->mode == NAV_VIEW);
    press(&x, ch('?'), R, C);
    CHECK(press(&x, ch(']'), R, C) == false && n->file == 0);
    press(&x, ch('?'), R, C);
    press(&x, K(KEY_EOF, 0), R, C);
    CHECK(n->quit);
    fx_close(&x);
}

/* ---- rendering ---- */

#define MAXR 40

typedef struct {
    Buf raw[MAXR];
    Buf plain[MAXR];
    int seen[MAXR];
    int ok;
} Scr;

static void scr_free(Scr *s) {
    for (int i = 0; i < MAXR; i++) {
        buf_free(&s->raw[i]);
        buf_free(&s->plain[i]);
    }
}

static void scr_parse(Scr *s, const Buf *f) {
    for (int i = 0; i < MAXR; i++) {
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
                int rown = atoi(f->data + i + 2);
                if (rown < 1 || rown > MAXR) { s->ok = 0; return; }
                cur = rown - 1;
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

static int geometry_ok(const Scr *s, int rows, int cols) {
    if (!s->ok) return 0;
    for (int r = 1; r <= rows; r++) {
        if (!s->seen[r - 1]) return 0;
        const Buf *p = &s->plain[r - 1];
        if (p->len == 0) continue;
        if (text_width(p->data, p->len) != (size_t)cols) return 0;
    }
    return 1;
}

static int starts(const Scr *s, int row, const char *prefix) {
    const Buf *p = &s->plain[row - 1];
    size_t n = strlen(prefix);
    return p->len >= n && memcmp(p->data, prefix, n) == 0;
}

static int ends(const Scr *s, int row, const char *suffix) {
    const Buf *p = &s->plain[row - 1];
    size_t n = strlen(suffix);
    return p->len >= n && memcmp(p->data + p->len - n, suffix, n) == 0;
}

static int has(const Scr *s, int row, const char *needle) {
    return strstr(s->plain[row - 1].data, needle) != NULL;
}

static int raw_has(const Scr *s, int row, const char *needle) {
    return strstr(s->raw[row - 1].data, needle) != NULL;
}

/* Text drawn right after each occurrence of a style (up to the next escape). */
static void marked_text(const Buf *raw, const char *style, Buf *out) {
    const char *p = raw->data;
    for (;;) {
        const char *hit = strstr(p, style);
        if (!hit) return;
        const char *m = strchr(hit, 'm');
        if (!m) return;
        m++;
        const char *e = strchr(m, '\x1b');
        buf_append(out, m, e ? (size_t)(e - m) : strlen(m));
        p = m;
    }
}

#define MATCH_T "48;2;214;178;62"
#define SEL_T "48;2;70;90;130"

typedef struct {
    Patch p;
    Model m;
    char *buf;
    size_t n;
    int built;
} Rx;

static int rx_open(Rx *x, const char *s, size_t n, size_t idx) {
    ParseError err;
    x->buf = heap(s, n);
    x->n = n;
    x->built = 0;
    int rc = patch_parse(&x->p, x->buf, n, &err);
    if (rc == 0 && idx < x->p.files.len) {
        model_build(&x->m, &x->p, vec_at(&x->p.files, idx));
        x->built = 1;
    }
    return x->built;
}

static void rx_close(Rx *x) {
    if (x->built) model_free(&x->m);
    patch_free(&x->p);
    free(x->buf);
}

static void base_view(const Rx *x, RenderView *v, int rows, int cols) {
    memset(v, 0, sizeof *v);
    v->model = &x->m;
    v->patch = &x->p;
    v->file_count = x->p.files.len;
    v->rows = rows;
    v->cols = cols;
    v->truecolor = true;
}

static void draw(const RenderView *v, Scr *s) {
    Buf f;
    buf_init(&f);
    render_frame(&f, v);
    scr_parse(s, &f);
    buf_free(&f);
}

static void test_render_picker(void) {
    Buf src;
    buf_init(&src);
    make_patch(&src);
    Rx x;
    REQUIRE(rx_open(&x, src.data, src.len, 0));
    Scr s;
    RenderView v;

    base_view(&x, &v, 12, 50);
    v.picker = true;
    v.pick = 1;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 12, 50));
    CHECK(starts(&s, 1, " Files (3)"));
    CHECK(starts(&s, 2, " * M  a.c"));   /* the open file is starred */
    CHECK(ends(&s, 2, "  +2 -2 "));
    CHECK(starts(&s, 3, "   M  b.c"));
    CHECK(starts(&s, 4, "   M  c.c"));
    CHECK(ends(&s, 4, "  +1 -0 "));
    CHECK(s.plain[4].len == 0);          /* below the list: erased */
    CHECK(raw_has(&s, 3, SEL_T));        /* the selected entry stands out */
    CHECK(!raw_has(&s, 2, SEL_T) && !raw_has(&s, 4, SEL_T));
    CHECK(starts(&s, 12, " j/k move   Enter open   Esc close"));
    CHECK(ends(&s, 12, " READ-ONLY "));
    scr_free(&s);

    /* scrolled list and a selection that follows the open file */
    v.file_idx = 2;
    v.pick = 2;
    v.pick_top = 1;
    draw(&v, &s);
    CHECK(starts(&s, 2, "   M  b.c"));
    CHECK(starts(&s, 3, " * M  c.c"));
    CHECK(raw_has(&s, 3, SEL_T));
    CHECK(s.plain[3].len == 0);
    scr_free(&s);

    /* a list start past the end is just empty */
    v.pick_top = 99;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 12, 50));
    CHECK(s.plain[1].len == 0);
    scr_free(&s);

    /* without a patch the picker cannot be drawn: the diff is shown */
    v.pick_top = 0;
    v.patch = NULL;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 12, 50));
    CHECK(starts(&s, 1, " 3/3 files"));
    scr_free(&s);

    /* every size is safe and exactly as wide as the screen */
    v.patch = &x.p;
    for (int rows = 1; rows <= 7; rows++) {
        for (int cols = 1; cols <= 40; cols++) {
            v.rows = rows;
            v.cols = cols;
            v.mono = cols % 2;
            draw(&v, &s);
            CHECK(geometry_ok(&s, rows, cols));
            scr_free(&s);
        }
    }
    rx_close(&x);
    buf_free(&src);
}

static void test_render_help(void) {
    Buf src;
    buf_init(&src);
    make_patch(&src);
    Rx x;
    REQUIRE(rx_open(&x, src.data, src.len, 0));
    Scr s;
    RenderView v;
    base_view(&x, &v, 24, 80);
    v.help = true;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 24, 80));
    CHECK(starts(&s, 1, " Help"));
    CHECK(starts(&s, 2, " Moving"));
    CHECK(has(&s, 3, "scroll one line"));
    CHECK(has(&s, 12, "file list"));
    CHECK(has(&s, 15, "search the current file"));
    CHECK(starts(&s, 24, " any key closes the help"));
    CHECK(ends(&s, 24, " READ-ONLY "));
    CHECK(!has(&s, 4, "alpha")); /* no diff rows behind the help */
    scr_free(&s);

    /* a short window shows the top of the page; a tall one pads the rest */
    v.rows = 6;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 6, 80));
    CHECK(starts(&s, 2, " Moving"));
    scr_free(&s);
    v.rows = 40;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 40, 80));
    scr_free(&s);

    /* the help wins over the file list */
    v.rows = 24;
    v.picker = true;
    draw(&v, &s);
    CHECK(starts(&s, 1, " Help"));
    scr_free(&s);
    rx_close(&x);
    buf_free(&src);
}

static void test_render_search(void) {
    Buf src;
    buf_init(&src);
    make_patch(&src);
    Rx x;
    REQUIRE(rx_open(&x, src.data, src.len, 0));
    Scr s;
    RenderView v;
    Buf t;
    buf_init(&t);

    /* rows: screen row r shows model row r-2 */
    base_view(&x, &v, 14, 60);
    v.query = "one";
    v.query_len = 3;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 14, 60));
    marked_text(&s.raw[2], MATCH_T, &t);        /* "alpha one" */
    CHECK(strcmp(t.data, "one") == 0);
    buf_clear(&t);
    marked_text(&s.raw[5], MATCH_T, &t);        /* "gamma one" */
    CHECK(strcmp(t.data, "one") == 0);
    buf_clear(&t);
    CHECK(!raw_has(&s, 4, MATCH_T));            /* "Beta two": no match */
    CHECK(has(&s, 3, "alpha one"));             /* text itself is unchanged */
    scr_free(&s);

    /* smart case: a capital makes it exact */
    v.query = "Beta";
    v.query_len = 4;
    draw(&v, &s);
    marked_text(&s.raw[3], MATCH_T, &t);
    CHECK(strcmp(t.data, "Beta") == 0);
    CHECK(!raw_has(&s, 5, MATCH_T));            /* "beta three" is not "Beta" */
    buf_clear(&t);
    scr_free(&s);
    v.query = "beta";
    v.query_len = 4;
    draw(&v, &s);
    CHECK(raw_has(&s, 3, MATCH_T) == 0);        /* gutter/other rows */
    CHECK(raw_has(&s, 4, MATCH_T) && raw_has(&s, 5, MATCH_T));
    scr_free(&s);

    /* hunk rows are searchable and highlighted */
    v.query = "main";
    v.query_len = 4;
    draw(&v, &s);
    marked_text(&s.raw[1], MATCH_T, &t);
    CHECK(strcmp(t.data, "main") == 0);
    CHECK(has(&s, 2, "@@ -1,3 +1,3 @@ int main"));
    buf_clear(&t);
    scr_free(&s);

    /* a match wins over the changed-word marks of the same row */
    v.query = "three";
    v.query_len = 5;
    draw(&v, &s);
    marked_text(&s.raw[4], MATCH_T, &t);
    CHECK(strcmp(t.data, "three") == 0);
    buf_clear(&t);
    scr_free(&s);

    /* the padding after a match is not highlighted */
    v.query = "gamma one";
    v.query_len = 9;
    draw(&v, &s);
    {
        const char *raw = s.raw[5].data, *last = NULL;
        for (const char *p = raw; (p = strstr(p, MATCH_T)) != NULL; p++) last = p;
        CHECK(last != NULL);
        int esc = 0;
        for (const char *p = last ? last + 1 : ""; *p; p++)
            if (*p == '\x1b') esc++;
        CHECK(esc >= 2);
    }
    scr_free(&s);

    /* no query, an empty one and an unmatched one draw nothing special */
    const char *none[] = {NULL, "", "zzzz"};
    for (int i = 0; i < 3; i++) {
        v.query = none[i];
        v.query_len = none[i] ? strlen(none[i]) : 0;
        draw(&v, &s);
        for (int r = 2; r <= 10; r++) CHECK(!raw_has(&s, r, MATCH_T));
        scr_free(&s);
    }

    /* horizontal scrolling and tiny windows keep every row the right width */
    v.query = "one";
    v.query_len = 3;
    for (int cols = 1; cols <= 40; cols++) {
        for (size_t left = 0; left < 12; left += 2) {
            v.cols = cols;
            v.left = left;
            v.mono = cols % 2;
            draw(&v, &s);
            CHECK(geometry_ok(&s, 14, cols));
            scr_free(&s);
        }
    }
    buf_free(&t);
    rx_close(&x);
    buf_free(&src);
}

static void test_render_bar(void) {
    Buf src;
    buf_init(&src);
    make_patch(&src);
    Rx x;
    REQUIRE(rx_open(&x, src.data, src.len, 0));
    Scr s;
    RenderView v;
    base_view(&x, &v, 12, 80);

    draw(&v, &s);
    CHECK(starts(&s, 12, " q quit  j/k scroll"));
    CHECK(has(&s, 12, "f files"));
    CHECK(has(&s, 12, "/ find"));
    CHECK(has(&s, 12, "? help"));
    CHECK(ends(&s, 12, " READ-ONLY  100% "));
    scr_free(&s);

    v.status = "/one  1/4";
    draw(&v, &s);
    CHECK(starts(&s, 12, " /one  1/4"));
    CHECK(!has(&s, 12, "q quit"));
    scr_free(&s);

    v.status = "";
    draw(&v, &s);
    CHECK(starts(&s, 12, " q quit"));
    scr_free(&s);

    v.typing = true;
    v.input = "ab";
    v.input_len = 2;
    draw(&v, &s);
    CHECK(starts(&s, 12, "/ab_"));
    CHECK(ends(&s, 12, " READ-ONLY  100% "));
    CHECK(geometry_ok(&s, 12, 80));
    scr_free(&s);

    v.input = "\xc3\xa9\xe6\x97\xa5";
    v.input_len = 5;
    draw(&v, &s);
    CHECK(starts(&s, 12, "/\xc3\xa9\xe6\x97\xa5_"));
    CHECK(geometry_ok(&s, 12, 80));
    scr_free(&s);

    /* the typed text wins over a leftover status */
    v.status = "Not found: x";
    draw(&v, &s);
    CHECK(starts(&s, 12, "/"));
    scr_free(&s);

    /* a very long input is clipped, never pushing the right side out */
    char longin[200];
    memset(longin, 'a', sizeof longin);
    v.input = longin;
    v.input_len = sizeof longin;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 12, 80));
    CHECK(ends(&s, 12, " READ-ONLY  100% "));
    scr_free(&s);

    /* hostile bytes in the message cannot reach the terminal */
    v.typing = false;
    v.status = "\x1b[2J\x07\xff weird";
    draw(&v, &s);
    CHECK(s.ok);
    CHECK(geometry_ok(&s, 12, 80));
    CHECK(has(&s, 12, "\xe2\x90\x9b[2J"));
    scr_free(&s);
    v.typing = true;
    v.input = "\x1b[31m\x01";
    v.input_len = 6;
    draw(&v, &s);
    CHECK(s.ok && geometry_ok(&s, 12, 80));
    scr_free(&s);

    rx_close(&x);
    buf_free(&src);
}

static int no_color_codes(const Scr *s, int rows) {
    for (int r = 1; r <= rows; r++) {
        const char *p = s->raw[r - 1].data;
        if (strstr(p, "38;2;") || strstr(p, "38;5;") || strstr(p, "48;")) return 0;
        /* only 0 (reset), 1 (bold), 2 (dim), 7 (reverse) and 2K are used */
        for (const char *q = strstr(p, "\x1b["); q != NULL; q = strstr(q + 2, "\x1b[")) {
            for (const char *c = q + 2; *c != 'm' && *c != 'K'; c++)
                if (*c == '\0' || !strchr("0127;", *c)) return 0;
        }
    }
    return 1;
}

static void test_render_mono(void) {
    static const char P[] =
        "--- a/f\n+++ b/f\n@@ -9,3 +9,3 @@\n keep\n-int a = 10;\n+int a = 20;\n tail\n";
    Rx x;
    Scr s;
    RenderView v;
    REQUIRE(rx_open(&x, P, sizeof P - 1, 0));

    base_view(&x, &v, 12, 60);
    v.mono = true;
    draw(&v, &s);
    CHECK(geometry_ok(&s, 12, 60));
    CHECK(no_color_codes(&s, 12));
    /* signs in the first gutter column; the numbers are 2 digits wide */
    CHECK(starts(&s, 3, "  9  9 keep"));
    CHECK(starts(&s, 4, "-10    int a = 10;"));
    CHECK(starts(&s, 5, "+   10 int a = 20;"));
    CHECK(starts(&s, 6, " 11 11 tail"));
    Buf t;
    buf_init(&t);
    marked_text(&s.raw[3], "\x1b[0;7m", &t);   /* changed words: reverse video */
    CHECK(strcmp(t.data, "10") == 0);
    buf_clear(&t);
    marked_text(&s.raw[4], "\x1b[0;7m", &t);
    CHECK(strcmp(t.data, "20") == 0);
    buf_free(&t);
    CHECK(raw_has(&s, 1, "\x1b[0;1;7m"));      /* bold reverse header */
    scr_free(&s);

    /* search matches, the file list and the help are readable too */
    v.query = "int";
    v.query_len = 3;
    draw(&v, &s);
    CHECK(raw_has(&s, 4, "\x1b[0;1;7m"));
    CHECK(no_color_codes(&s, 12));
    scr_free(&s);
    v.query = NULL;
    v.query_len = 0;
    v.picker = true;
    draw(&v, &s);
    CHECK(no_color_codes(&s, 12));
    CHECK(raw_has(&s, 2, "\x1b[0;7m")); /* selection */
    scr_free(&s);
    v.picker = false;
    v.help = true;
    draw(&v, &s);
    CHECK(no_color_codes(&s, 12));
    scr_free(&s);

    /* the same frame in color has a sign-free gutter */
    v.help = false;
    v.mono = false;
    draw(&v, &s);
    CHECK(starts(&s, 4, " 10    int a = 10;"));
    scr_free(&s);
    rx_close(&x);
}

/* Every combination of screens, with hostile text in every string, over
 * mutated patches: always exactly the screen size, nothing but our own
 * escape sequences. */
static void test_render_fuzz(void) {
    Buf base, m;
    buf_init(&base);
    make_patch(&base);
    buf_init(&m);
    int rendered = 0;
    for (int iter = 0; iter < 12000; iter++) {
        mutate(&m, base.data, base.len);
        Rx x;
        if (!rx_open(&x, m.data, m.len, 0)) {
            patch_free(&x.p);
            free(x.buf);
            continue;
        }
        char hostile[24];
        for (size_t i = 0; i < sizeof hostile; i++) hostile[i] = (char)(rnd() % 256);
        RenderView v;
        base_view(&x, &v, (int)(1 + rnd() % 30), (int)(1 + rnd() % 90));
        v.file_idx = rnd() % x.p.files.len;
        v.top = rnd() % (x.m.rows.len + 2);
        v.left = rnd() % 20;
        v.truecolor = rnd() & 1;
        v.mono = rnd() % 4 == 0;
        v.picker = rnd() % 4 == 0;
        v.help = rnd() % 8 == 0;
        v.pick = rnd() % (x.p.files.len + 1);
        v.pick_top = rnd() % (x.p.files.len + 1);
        v.typing = rnd() % 5 == 0;
        v.input = hostile;
        v.input_len = rnd() % sizeof hostile;
        v.query = rnd() & 1 ? hostile : "one";
        v.query_len = rnd() % 5;
        v.status = rnd() % 3 == 0 ? "\x1b[1mhi\x07" : NULL;
        Scr s;
        draw(&v, &s);
        CHECK(geometry_ok(&s, v.rows, v.cols));
        rendered++;
        scr_free(&s);
        rx_close(&x);
    }
    CHECK(rendered > 500);
    buf_free(&base);
    buf_free(&m);
}

int main(void) {
    printf("test_polish\n");
    test_search_find();
    test_search_spans();
    test_search_random();
    test_nav_search();
    test_nav_search_case();
    test_nav_search_scroll();
    test_nav_search_edge();
    test_nav_search_input();
    test_nav_picker();
    test_nav_picker_scroll();
    test_nav_help();
    test_render_picker();
    test_render_help();
    test_render_search();
    test_render_bar();
    test_render_mono();
    test_render_fuzz();
    TEST_DONE();
}
