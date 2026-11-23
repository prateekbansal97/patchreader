/* Large-input benchmark and regression gate. Builds big synthetic patches in
 * memory and times each stage of the read path. Exits non-zero when a stage
 * exceeds its (deliberately generous) limit.
 *
 *   make bench
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "model.h"
#include "nav.h"
#include "patch.h"
#include "render.h"
#include "util.h"

static int g_failed;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void gate(const char *what, double secs, double limit) {
    int bad = secs > limit;
    printf("  %-26s %8.4f s  (limit %.1f)%s\n", what, secs, limit, bad ? "  TOO SLOW" : "");
    if (bad) g_failed = 1;
}

static Key key(KeyType t, uint32_t ch) {
    Key k;
    k.type = t;
    k.ch = ch;
    return k;
}

/* Parses, builds the first file's model, renders frames, searches and jumps. */
static void run(const char *name, const Buf *src, double far_left_limit) {
    printf("%s (%zu KB)\n", name, src->len >> 10);
    double t = now();
    Patch p;
    ParseError err;
    if (patch_parse(&p, src->data, src->len, &err) != 0) {
        printf("  parse failed: %s\n", err.msg);
        g_failed = 1;
        patch_free(&p);
        return;
    }
    gate("parse", now() - t, 2.0);

    t = now();
    Model m;
    model_build(&m, &p, vec_at(&p.files, 0));
    gate("model", now() - t, 2.0);
    printf("  %zu files, %zu rows in the first, widest line %zu columns\n", p.files.len, m.rows.len,
           m.max_cols);

    RenderView v;
    memset(&v, 0, sizeof v);
    v.model = &m;
    v.patch = &p;
    v.file_count = p.files.len;
    v.rows = 50;
    v.cols = 200;
    v.truecolor = true;
    Buf f;
    buf_init(&f);
    double worst = 0;
    for (size_t i = 0; i < 20; i++) {
        v.top = (m.rows.len / 20) * i;
        buf_clear(&f);
        t = now();
        render_frame(&f, &v);
        double d = now() - t;
        if (d > worst) worst = d;
    }
    gate("render frame (worst of 20)", worst, 0.5);

    if (m.max_cols > 1000) { /* a huge line: the far end is the slow place */
        v.top = 0;
        v.left = m.max_cols - 100;
        buf_clear(&f);
        t = now();
        render_frame(&f, &v);
        gate("render at far right", now() - t, far_left_limit);
    }

    Nav nav;
    nav_init(&nav, p.files.len);
    t = now();
    nav_key(&nav, key(KEY_CHAR, '/'), &m, 50, 200);
    nav_key(&nav, key(KEY_CHAR, 'z'), &m, 50, 200);
    nav_key(&nav, key(KEY_CHAR, 'q'), &m, 50, 200);
    nav_key(&nav, key(KEY_ENTER, 0), &m, 50, 200); /* not found: scans every row */
    gate("search (no match)", now() - t, 2.0);
    t = now();
    nav_key(&nav, key(KEY_CHAR, '}'), &m, 50, 200);
    nav_key(&nav, key(KEY_CHAR, 'G'), &m, 50, 200);
    nav_key(&nav, key(KEY_CHAR, '{'), &m, 50, 200);
    gate("hunk jumps and G", now() - t, 0.5);

    if (p.files.len > 1) {
        t = now();
        for (size_t i = 0; i < 20; i++) {
            model_free(&m);
            model_build(&m, &p, vec_at(&p.files, i % p.files.len));
        }
        gate("20 file switches", now() - t, 1.0);
    }
    buf_free(&f);
    model_free(&m);
    patch_free(&p);
}

int main(void) {
    char s[256];
    Buf b;
    buf_init(&b);

    /* one file of 200,000 added lines */
    buf_puts(&b, "diff --git a/big.c b/big.c\nnew file mode 100644\n--- /dev/null\n+++ b/big.c\n@@ -0,0 +1,200000 @@\n");
    for (int i = 0; i < 200000; i++) {
        snprintf(s, sizeof s, "+int value_%d = compute(%d, \"text %d\");\n", i, i, i);
        buf_puts(&b, s);
    }
    run("one big file", &b, 0.5);

    /* 5,000 small files */
    buf_clear(&b);
    for (int k = 0; k < 5000; k++) {
        snprintf(s, sizeof s, "diff --git a/d/f%d.c b/d/f%d.c\n--- a/d/f%d.c\n+++ b/d/f%d.c\n@@ -1,20 +1,20 @@\n", k, k, k, k);
        buf_puts(&b, s);
        for (int i = 0; i < 10; i++) buf_puts(&b, " ctx line\n");
        for (int i = 0; i < 10; i++) {
            snprintf(s, sizeof s, "-old line %d value %d\n", i, k);
            buf_puts(&b, s);
        }
        for (int i = 0; i < 10; i++) {
            snprintf(s, sizeof s, "+new line %d value %d\n", i, k + 1);
            buf_puts(&b, s);
        }
    }
    run("many files", &b, 0.5);

    /* 100,000 removed lines paired with 100,000 added lines */
    buf_clear(&b);
    buf_puts(&b, "--- a/x\n+++ b/x\n@@ -1,100000 +1,100000 @@\n");
    for (int i = 0; i < 100000; i++) {
        snprintf(s, sizeof s, "-    result_%d = old_function(arg_%d, %d);\n", i, i, i);
        buf_puts(&b, s);
    }
    for (int i = 0; i < 100000; i++) {
        snprintf(s, sizeof s, "+    result_%d = new_function(arg_%d, %d);\n", i, i, i + 1);
        buf_puts(&b, s);
    }
    run("big paired block", &b, 0.5);

    /* long lines near the word-diff limit, all paired */
    buf_clear(&b);
    buf_puts(&b, "--- a/x\n+++ b/x\n@@ -1,500 +1,500 @@\n");
    for (int i = 0; i < 500; i++) {
        buf_putc(&b, '-');
        for (int w = 0; w < 600; w++) {
            snprintf(s, sizeof s, "word%d ", i);
            buf_puts(&b, s);
        }
        buf_putc(&b, '\n');
    }
    for (int i = 0; i < 500; i++) {
        buf_putc(&b, '+');
        for (int w = 0; w < 600; w++) {
            snprintf(s, sizeof s, "word%d ", i);
            buf_puts(&b, s);
        }
        buf_puts(&b, "X\n");
    }
    run("long paired lines", &b, 0.5);

    /* a 24 MB single line on each side */
    buf_clear(&b);
    buf_puts(&b, "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-");
    for (int w = 0; w < 3000000; w++) buf_puts(&b, "abc def ");
    buf_puts(&b, "\n+");
    for (int w = 0; w < 3000000; w++) buf_puts(&b, "abc deg ");
    buf_puts(&b, "\n");
    run("one 24 MB line", &b, 1.0);

    buf_free(&b);
    printf(g_failed ? "bench: FAILED\n" : "bench: ok\n");
    return g_failed;
}
