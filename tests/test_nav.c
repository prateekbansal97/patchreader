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
#include "util.h"

#define REQUIRE(cond)     \
    do {                  \
        int ok_ = (cond); \
        CHECK(ok_);       \
        if (!ok_) return; \
    } while (0)

/* A three-file patch:
 *   a.c: three hunks of 4 rows each (hunk, context, del, add) = 12 rows,
 *        hunk rows at 0, 4 and 8
 *   b.c: one hunk, 3 rows
 *   c.c: one hunk with a very long added line (80 columns) */
static void make_patch(Buf *b) {
    buf_clear(b);
    buf_puts(b, "diff --git a/a.c b/a.c\n--- a/a.c\n+++ b/a.c\n");
    static const int starts[] = {1, 20, 40};
    for (int i = 0; i < 3; i++) {
        char h[64];
        snprintf(h, sizeof h, "@@ -%d,2 +%d,2 @@\n", starts[i], starts[i]);
        buf_puts(b, h);
        buf_puts(b, " ctx\n-del\n+add\n");
    }
    buf_puts(b, "diff --git a/b.c b/b.c\n--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-x\n+y\n");
    buf_puts(b, "diff --git a/c.c b/c.c\n--- a/c.c\n+++ b/c.c\n@@ -0,0 +1 @@\n+");
    for (int i = 0; i < 80; i++) buf_putc(b, (char)('a' + i % 26));
    buf_putc(b, '\n');
}

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

static int fx_open(Fx *x, size_t file) {
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
    x->nav.file = file;
    fx_load(x, file);
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

/* Sends a key like main() does: rebuilds the model when the file changes. */
static bool press(Fx *x, Key k, int rows, int cols) {
    bool changed = nav_key(&x->nav, k, &x->m, rows, cols);
    if (changed) fx_load(x, x->nav.file);
    return changed;
}

/* ---- tests ---- */

static void test_setup(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    CHECK(x.p.files.len == 3);
    CHECK(x.m.rows.len == 12);
    CHECK(x.nav.file == 0 && x.nav.top == 0 && x.nav.left == 0 && !x.nav.quit);
    CHECK(x.nav.file_count == 3);
    fx_close(&x);
}

static void test_render_helpers(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0)); /* line numbers <= 41: two digits each */
    CHECK(render_gutter_width(&x.m) == 2 + 2 + 3);
    CHECK(render_max_left(&x.m, 80) == 0); /* "ctx" fits */
    fx_close(&x);

    REQUIRE(fx_open(&x, 2)); /* one digit each, 80-column line */
    CHECK(render_gutter_width(&x.m) == 5);
    CHECK(x.m.max_cols == 80);
    CHECK(render_max_left(&x.m, 40) == 80 - 35);
    CHECK(render_max_left(&x.m, 85) == 0);
    CHECK(render_max_left(&x.m, 84) == 1);
    CHECK(render_max_left(&x.m, 5) == 0); /* no room for text at all */
    CHECK(render_max_left(&x.m, 1) == 0);
    CHECK(render_max_left(&x.m, 0) == 0);
    CHECK(render_max_left(&x.m, -4) == 0);
    fx_close(&x);
}

static void test_vertical(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    const int R = 6, C = 40; /* body 4 rows, page 3, half 2, max_top 8 */
    Nav *n = &x.nav;

    press(&x, ch('j'), R, C); CHECK(n->top == 1);
    press(&x, K(KEY_DOWN, 0), R, C); CHECK(n->top == 2);
    press(&x, K(KEY_ENTER, 0), R, C); CHECK(n->top == 3);
    press(&x, ch('k'), R, C); CHECK(n->top == 2);
    press(&x, K(KEY_UP, 0), R, C); CHECK(n->top == 1);
    press(&x, K(KEY_PGDN, 0), R, C); CHECK(n->top == 4);
    press(&x, ch(' '), R, C); CHECK(n->top == 7);
    press(&x, K(KEY_PGUP, 0), R, C); CHECK(n->top == 4);
    press(&x, ch('b'), R, C); CHECK(n->top == 1);
    press(&x, K(KEY_CTRL, 'd'), R, C); CHECK(n->top == 3);
    press(&x, K(KEY_CTRL, 'u'), R, C); CHECK(n->top == 1);
    press(&x, ch('G'), R, C); CHECK(n->top == 8);
    press(&x, ch('j'), R, C); CHECK(n->top == 8);          /* clamped at the end */
    press(&x, K(KEY_PGDN, 0), R, C); CHECK(n->top == 8);
    press(&x, K(KEY_CTRL, 'd'), R, C); CHECK(n->top == 8);
    press(&x, ch('g'), R, C); CHECK(n->top == 0);
    press(&x, ch('k'), R, C); CHECK(n->top == 0);          /* clamped at the start */
    press(&x, K(KEY_PGUP, 0), R, C); CHECK(n->top == 0);
    press(&x, K(KEY_END, 0), R, C); CHECK(n->top == 8);
    press(&x, K(KEY_HOME, 0), R, C); CHECK(n->top == 0);

    /* none of those changed the file or asked to quit */
    CHECK(n->file == 0 && !n->quit);

    /* whole file fits: nothing to scroll */
    press(&x, ch('j'), 24, C); CHECK(n->top == 0);
    press(&x, ch('G'), 24, C); CHECK(n->top == 0);
    fx_close(&x);
}

static void test_hunks(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    const int R = 6, C = 40;
    Nav *n = &x.nav;

    press(&x, ch('}'), R, C); CHECK(n->top == 4);
    press(&x, ch('}'), R, C); CHECK(n->top == 8);
    press(&x, ch('}'), R, C); CHECK(n->top == 8); /* no later hunk */
    press(&x, ch('{'), R, C); CHECK(n->top == 4);
    press(&x, ch('{'), R, C); CHECK(n->top == 0);
    press(&x, ch('{'), R, C); CHECK(n->top == 0); /* no earlier hunk */

    press(&x, ch('J'), R, C); CHECK(n->top == 4);
    n->top = 6;                                   /* between hunks */
    press(&x, ch('K'), R, C); CHECK(n->top == 4);
    n->top = 6;
    press(&x, ch('}'), R, C); CHECK(n->top == 8);
    n->top = 1;
    press(&x, ch('{'), R, C); CHECK(n->top == 0);

    /* a hunk below the last reachable top is clamped to it */
    n->top = 0;
    press(&x, ch('}'), 12, C); /* body 10, max_top 2 */
    CHECK(n->top == 2);
    press(&x, ch('}'), 12, C);
    CHECK(n->top == 2);
    press(&x, ch('{'), 12, C);
    CHECK(n->top == 0);

    /* files without hunk rows (only a note) have nowhere to jump */
    fx_close(&x);
    static const char BIN[] =
        "diff --git a/i.png b/i.png\nnew file mode 100644\nBinary files /dev/null and b/i.png differ\n";
    Patch p;
    ParseError err;
    REQUIRE(patch_parse(&p, BIN, sizeof BIN - 1, &err) == 0);
    Model m;
    model_build(&m, &p, vec_at(&p.files, 0));
    Nav nv;
    nav_init(&nv, 1);
    CHECK(!nav_key(&nv, ch('}'), &m, 10, 40) && nv.top == 0);
    CHECK(!nav_key(&nv, ch('{'), &m, 10, 40) && nv.top == 0);
    model_free(&m);
    patch_free(&p);
}

static void test_files(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    const int R = 6, C = 40;
    Nav *n = &x.nav;

    n->top = 5;
    n->left = 0;
    CHECK(press(&x, ch(']'), R, C) == true);
    CHECK(n->file == 1 && n->top == 0 && n->left == 0);
    CHECK(x.m.file == vec_at(&x.p.files, 1));
    CHECK(x.m.rows.len == 3);

    CHECK(press(&x, K(KEY_TAB, 0), R, C) == true);
    CHECK(n->file == 2);
    CHECK(x.m.max_cols == 80);

    CHECK(press(&x, ch(']'), R, C) == false); /* already last */
    CHECK(press(&x, K(KEY_TAB, 0), R, C) == false);
    CHECK(n->file == 2);

    n->left = 20;
    CHECK(press(&x, ch('['), R, C) == true);
    CHECK(n->file == 1 && n->left == 0 && n->top == 0);
    CHECK(press(&x, K(KEY_BTAB, 0), R, C) == true);
    CHECK(n->file == 0);
    CHECK(press(&x, ch('['), R, C) == false); /* already first */
    CHECK(press(&x, K(KEY_BTAB, 0), R, C) == false);
    CHECK(n->file == 0);

    /* other keys never report a file change */
    CHECK(press(&x, ch('j'), R, C) == false);
    CHECK(press(&x, ch('q'), R, C) == false);
    fx_close(&x);

    /* a single-file patch has nowhere to go */
    static const char ONE[] = "--- a/f\n+++ b/f\n@@ -1 +1 @@\n-a\n+b\n";
    Patch p;
    ParseError err;
    REQUIRE(patch_parse(&p, ONE, sizeof ONE - 1, &err) == 0);
    Model m;
    model_build(&m, &p, vec_at(&p.files, 0));
    Nav nv;
    nav_init(&nv, p.files.len);
    CHECK(!nav_key(&nv, ch(']'), &m, 10, 40));
    CHECK(!nav_key(&nv, ch('['), &m, 10, 40));
    CHECK(!nav_key(&nv, K(KEY_TAB, 0), &m, 10, 40));
    CHECK(!nav_key(&nv, K(KEY_BTAB, 0), &m, 10, 40));
    CHECK(nv.file == 0);
    model_free(&m);
    patch_free(&p);
}

static void test_horizontal(void) {
    Fx x;
    REQUIRE(fx_open(&x, 2)); /* max_left at 40 columns is 45 */
    const int R = 8, C = 40;
    Nav *n = &x.nav;

    press(&x, ch('l'), R, C); CHECK(n->left == 8);
    press(&x, K(KEY_RIGHT, 0), R, C); CHECK(n->left == 16);
    press(&x, ch('h'), R, C); CHECK(n->left == 8);
    press(&x, K(KEY_LEFT, 0), R, C); CHECK(n->left == 0);
    press(&x, ch('h'), R, C); CHECK(n->left == 0);
    for (int i = 0; i < 20; i++) press(&x, ch('l'), R, C);
    CHECK(n->left == 45);
    press(&x, K(KEY_RIGHT, 0), R, C); CHECK(n->left == 45);
    press(&x, ch('0'), R, C); CHECK(n->left == 0);
    press(&x, ch('$'), R, C); CHECK(n->left == 45);
    press(&x, ch('h'), R, C); CHECK(n->left == 37);

    /* a wider screen needs less panning; nav_clamp pulls left back in */
    nav_clamp(n, &x.m, R, 70);
    CHECK(n->left == 15);
    nav_clamp(n, &x.m, R, 200);
    CHECK(n->left == 0);

    /* panning does not touch the vertical position */
    CHECK(n->top == 0);
    fx_close(&x);

    /* a file that fits never pans */
    REQUIRE(fx_open(&x, 0));
    press(&x, ch('l'), R, C); CHECK(x.nav.left == 0);
    press(&x, ch('$'), R, C); CHECK(x.nav.left == 0);
    fx_close(&x);
}

static void test_quit_and_ignored(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    const int R = 6, C = 40;

    Key none[] = {K(KEY_NONE, 0), K(KEY_RESIZE, 0), K(KEY_ESC, 0), K(KEY_BACKSPACE, 0),
                  K(KEY_DEL, 0),  K(KEY_CTRL, 'c'), K(KEY_CTRL, 'z'), ch('x'), ch('Q'), ch('?'),
                  K(KEY_CHAR, 0x1F600)};
    x.nav.top = 3;
    x.nav.left = 0;
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        CHECK(!press(&x, none[i], R, C));
        CHECK(x.nav.top == 3 && x.nav.file == 0 && !x.nav.quit);
    }

    press(&x, ch('q'), R, C);
    CHECK(x.nav.quit);
    fx_close(&x);

    REQUIRE(fx_open(&x, 0));
    press(&x, K(KEY_EOF, 0), R, C);
    CHECK(x.nav.quit);
    fx_close(&x);
}

static void test_clamp(void) {
    Fx x;
    REQUIRE(fx_open(&x, 0));
    x.nav.top = 1000;
    x.nav.left = 1000;
    nav_clamp(&x.nav, &x.m, 6, 40);
    CHECK(x.nav.top == 8 && x.nav.left == 0);

    x.nav.top = SIZE_MAX;
    nav_clamp(&x.nav, &x.m, 6, 40);
    CHECK(x.nav.top == 8);

    /* the window grows: top is pulled back so the last row stays at the bottom */
    nav_clamp(&x.nav, &x.m, 12, 40);
    CHECK(x.nav.top == 2);
    nav_clamp(&x.nav, &x.m, 24, 40);
    CHECK(x.nav.top == 0);

    /* positions near SIZE_MAX do not wrap on scrolling */
    x.nav.top = SIZE_MAX;
    CHECK(!nav_key(&x.nav, ch('j'), &x.m, 6, 40));
    CHECK(x.nav.top == 8);
    x.nav.top = SIZE_MAX;
    CHECK(!nav_key(&x.nav, ch('}'), &x.m, 6, 40));
    x.nav.top = SIZE_MAX;
    CHECK(!nav_key(&x.nav, ch('{'), &x.m, 6, 40));
    CHECK(x.nav.top == 8);
    fx_close(&x);
}

static void test_tiny_screens(void) {
    Fx x;
    REQUIRE(fx_open(&x, 2));
    Key keys[] = {ch('j'), ch('k'), ch(' '), ch('b'), ch('G'), ch('g'), ch('}'), ch('{'),
                  ch('l'), ch('h'), ch('$'), K(KEY_CTRL, 'd'), K(KEY_CTRL, 'u'),
                  K(KEY_PGDN, 0), K(KEY_PGUP, 0), K(KEY_END, 0), K(KEY_HOME, 0)};
    for (int rows = -1; rows <= 4; rows++) {
        for (int cols = -1; cols <= 8; cols++) {
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
                press(&x, keys[i], rows, cols);
                nav_clamp(&x.nav, &x.m, rows, cols);
                CHECK(x.nav.top <= render_max_top(&x.m, rows));
                CHECK(x.nav.left <= render_max_left(&x.m, cols));
            }
        }
    }
    fx_close(&x);
}

static void test_random_keys(void) {
    static const KeyType types[] = {KEY_NONE, KEY_CHAR, KEY_CTRL, KEY_UP, KEY_DOWN, KEY_LEFT,
                                    KEY_RIGHT, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_DEL,
                                    KEY_TAB, KEY_BTAB, KEY_ENTER, KEY_BACKSPACE, KEY_ESC,
                                    KEY_RESIZE, KEY_EOF};
    static const char chars[] = "jkhlqgGbn ]['{}JK0$xd";
    Fx x;
    REQUIRE(fx_open(&x, 0));
    for (int i = 0; i < 50000; i++) {
        Key k;
        k.type = types[rnd() % (sizeof types / sizeof types[0])];
        k.ch = (uint32_t)(unsigned char)chars[rnd() % (sizeof chars - 1)];
        int rows = (int)(1 + rnd() % 30);
        int cols = (int)(1 + rnd() % 100);
        if (rnd() % 50 == 0) {
            /* the window was resized between keys, as main() clamps */
            nav_clamp(&x.nav, &x.m, rows, cols);
        }
        x.nav.quit = false;
        bool before_changed_file = false;
        size_t old_file = x.nav.file;
        before_changed_file = press(&x, k, rows, cols);
        CHECK(x.nav.file < x.nav.file_count);
        CHECK(before_changed_file == (x.nav.file != old_file));
        if (before_changed_file) CHECK(x.nav.top == 0 && x.nav.left == 0);
        nav_clamp(&x.nav, &x.m, rows, cols);
        CHECK(x.nav.top <= render_max_top(&x.m, rows));
        CHECK(x.nav.left <= render_max_left(&x.m, cols));
    }
    fx_close(&x);
}

int main(void) {
    printf("test_nav\n");
    test_setup();
    test_render_helpers();
    test_vertical();
    test_hunks();
    test_files();
    test_horizontal();
    test_quit_and_ignored();
    test_clamp();
    test_tiny_screens();
    test_random_keys();
    TEST_DONE();
}
