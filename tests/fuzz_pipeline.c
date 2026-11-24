/* Long-running fuzzer for the whole read path: mutated patches go through
 * the parser, the model, the key handler and the renderer. Build with the
 * sanitizers (make fuzz); a crash or a broken invariant dumps the input to
 * fuzz-crash.diff.
 *
 *   fuzz_pipeline [seconds [seed]]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fuzz.h"
#include "model.h"
#include "nav.h"
#include "patch.h"
#include "render.h"
#include "screen.h"
#include "util.h"

static const char *const SEEDS[] = {
    /* git patch with mail header, several files and hunks */
    "From 1a2b Mon Sep 17 00:00:00 2001\nFrom: A <a@b.c>\nSubject: [PATCH] x\n\nmessage\n---\n"
    " f | 2 +-\n\n"
    "diff --git a/src/a.c b/src/a.c\nindex 111..222 100644\n--- a/src/a.c\n+++ b/src/a.c\n"
    "@@ -1,4 +1,4 @@ int main(void)\n keep\n-old line here\n+new line here\n tail\n x\n"
    "@@ -20,2 +20,3 @@\n a\n+b\n c\n"
    "diff --git a/b.txt b/b.txt\n--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-x\n+y\n-- \n2.40.0\n",
    /* new, deleted, renamed, mode change, binary */
    "diff --git a/n b/n\nnew file mode 100644\n--- /dev/null\n+++ b/n\n@@ -0,0 +1,2 @@\n+a\n+b\n"
    "diff --git a/d b/d\ndeleted file mode 100644\n--- a/d\n+++ /dev/null\n@@ -1 +0,0 @@\n-gone\n"
    "diff --git a/o b/p\nsimilarity index 90%\nrename from o\nrename to p\n"
    "diff --git a/m.sh b/m.sh\nold mode 100644\nnew mode 100755\n"
    "diff --git a/i.png b/i.png\nBinary files a/i.png and b/i.png differ\n",
    /* no newline markers, CRLF, tabs, wide and invalid UTF-8 */
    "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n-one\n\\ No newline at end of file\n+two\n\\ No newline at end of file\n",
    "--- a/f\r\n+++ b/f\r\n@@ -1 +1 @@\r\n-a\tb\r\n+a  b\r\n",
    "--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n \xe6\x97\xa5\xe6\x9c\xac \x1b[31m\n-caf\xc3\xa9 \xff\xfe\n+caf\xc3\xa9s \x01\n",
    /* plain diff with timestamps, quoted names, combined diff */
    "--- old/file.c\t2020-01-01 10:00:00.000000000 +0000\n+++ new/file.c\t2020-01-02 10:00:00.000000000 +0000\n"
    "@@ -1,3 +1,3 @@\n a\n-b\n+c\n d\n",
    "diff --git \"a/we\\tird\" \"b/we\\tird\"\n--- \"a/we\\tird\"\n+++ \"b/we\\tird\"\n@@ -1 +1 @@\n-a\n+b\n",
    "diff --cc f\n--- a/f\n+++ b/f\n@@@ -1,2 -1,2 +1,3 @@@\n  a\n++b\n  c\n",
    /* a long line and a big paired block */
    "--- a/x\n+++ b/x\n@@ -1,3 +1,3 @@\n-int count = compute_total(items, 10); // total of all the items in the list\n"
    "-return count;\n-x\n+long count = compute_total(items, 20); // total of all the items\n+return count + 1;\n+y\n",
};
#define NSEEDS (sizeof SEEDS / sizeof SEEDS[0])

static Buf g_input;
static unsigned long g_iter;
static uint32_t g_seed;

static void dump_input(void) {
    FILE *f = fopen("fuzz-crash.diff", "wb");
    if (!f) return;
    fwrite(g_input.data, 1, g_input.len, f);
    fclose(f);
    fprintf(stderr, "\nfuzz: iteration %lu (seed %u): input saved to fuzz-crash.diff (%zu bytes)\n",
            g_iter, g_seed, g_input.len);
}

/* Called by AddressSanitizer / UBSan before they abort. */
void __asan_on_error(void) { dump_input(); }

static void fail(const char *what) {
    fprintf(stderr, "fuzz: invariant broken: %s\n", what);
    dump_input();
    abort();
}

static Key random_key(void) {
    static const KeyType types[] = {KEY_CHAR, KEY_CHAR, KEY_CHAR, KEY_CHAR, KEY_CTRL, KEY_UP,
                                    KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN, KEY_HOME,
                                    KEY_END, KEY_DEL, KEY_TAB, KEY_BTAB, KEY_ENTER, KEY_BACKSPACE,
                                    KEY_ESC, KEY_RESIZE, KEY_NONE};
    static const char chars[] = "jkhlgGbn ]['{}JK0$f?/xeaz\x01";
    Key k;
    k.type = types[rnd() % (sizeof types / sizeof types[0])];
    k.ch = (uint32_t)(unsigned char)chars[rnd() % (sizeof chars - 1)];
    if (rnd() % 20 == 0) k.ch = rnd() % 0x110000u; /* any code point, valid or not */
    if (k.type == KEY_CTRL) k.ch = 'a' + rnd() % 26;
    return k;
}

static void exercise(const Patch *p) {
    size_t files = p->files.len;
    Model m;
    Nav nav;
    nav_init(&nav, files);
    History hist;
    history_init(&hist);
    nav.hist = &hist;
    model_build(&m, p, vec_at(&p->files, 0));
    Buf frame;
    buf_init(&frame);
    for (int step = 0; step < 80; step++) {
        int rows = (int)(1 + rnd() % 40), cols = (int)(1 + rnd() % 120);
        nav_clamp(&nav, &m, rows, cols);

        RenderView v;
        memset(&v, 0, sizeof v);
        v.model = &m;
        v.patch = p;
        v.file_count = files;
        v.file_idx = nav.file;
        v.top = nav.top;
        v.left = nav.left;
        v.rows = rows;
        v.cols = cols;
        v.truecolor = rnd() & 1;
        v.mono = rnd() % 5 == 0;
        v.picker = nav.mode == NAV_PICK;
        v.pick = nav.pick;
        v.pick_top = nav.pick_top;
        v.help = nav.mode == NAV_HELP;
        v.typing = nav.mode == NAV_SEARCH;
        v.input = nav.input;
        v.input_len = nav.input_len;
        v.query = nav.query;
        v.query_len = nav.query_len;
        v.status = nav.status;

        buf_clear(&frame);
        render_frame(&frame, &v);
        Scr s;
        scr_parse(&s, &frame);
        int ok = scr_geometry_ok(&s, rows, cols);
        scr_free(&s);
        if (!ok) fail("frame is not exactly the screen size / has foreign escapes");

        if (nav.top > render_max_top(&m, rows) || nav.left > render_max_left(&m, cols))
            fail("scroll position out of range after clamp");
        if (nav.file >= files || nav.pick >= files) fail("file index out of range");

        Key k = random_key();
        nav.quit = false;
        if (nav_key(&nav, k, &m, rows, cols)) {
            model_free(&m);
            model_build(&m, p, vec_at(&p->files, nav.file));
        }
        if (nav.input_len >= sizeof nav.input || nav.input[nav.input_len] != '\0')
            fail("search prompt not terminated");
        if (nav.query_len >= sizeof nav.query || nav.query[nav.query_len] != '\0')
            fail("search query not terminated");
        if (hist.count > HISTORY_MAX) fail("search history over its limit");
        if (nav.mode == NAV_SEARCH && nav.hist_pos != SIZE_MAX && nav.hist_pos >= hist.count)
            fail("history position out of range");
        nav.hist_dirty = false;
    }
    buf_free(&frame);
    model_free(&m);
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    g_seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : (uint32_t)time(NULL);
    printf("fuzz_pipeline: %.0f s, seed %u\n", seconds, g_seed);

    buf_init(&g_input);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long parsed = 0;
    for (g_iter = 0;; g_iter++) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) break;

        g_rng = (g_seed ^ (uint32_t)(g_iter * 2654435761u)) | 1u;
        if (rnd() % 25 == 0) { /* pure noise */
            size_t n = rnd() % 300;
            buf_clear(&g_input);
            for (size_t i = 0; i < n; i++) buf_putc(&g_input, (char)(rnd() & 0xff));
        } else {
            const char *seed = SEEDS[rnd() % NSEEDS];
            mutate(&g_input, seed, strlen(seed));
            if (rnd() % 3 == 0) { /* stack a few more rounds of damage */
                Buf tmp;
                buf_init(&tmp);
                for (int r = 0; r < 3; r++) {
                    mutate(&tmp, g_input.data, g_input.len);
                    buf_clear(&g_input);
                    buf_append(&g_input, tmp.data, tmp.len);
                }
                buf_free(&tmp);
            }
        }

        char *copy = xmalloc(g_input.len ? g_input.len : 1); /* exact size: overreads trip ASan */
        if (g_input.len) memcpy(copy, g_input.data, g_input.len);
        Patch p;
        ParseError err;
        if (patch_parse(&p, copy, g_input.len, &err) == 0) {
            parsed++;
            exercise(&p);
        }
        patch_free(&p);
        free(copy);
    }
    printf("fuzz_pipeline: %lu inputs, %lu parsed and exercised, no failures\n", g_iter, parsed);
    buf_free(&g_input);
    return 0;
}
