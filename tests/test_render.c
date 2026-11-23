#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "model.h"
#include "patch.h"
#include "render.h"
#include "text.h"
#include "util.h"

#define REQUIRE(cond)       \
    do {                    \
        int ok_ = (cond);   \
        CHECK(ok_);         \
        if (!ok_) return;   \
    } while (0)

#define MAXR 64

/* A frame split back into screen rows: raw keeps our own escape sequences,
 * plain has them removed. ok is cleared if the frame contains anything but
 * cursor moves (ESC[r;1H), SGR (ESC[..m) and line erase (ESC[2K). */
typedef struct {
    Buf raw[MAXR];
    Buf plain[MAXR];
    int seen[MAXR];
    int ok;
} Scr;

static void scr_init(Scr *s) {
    for (int i = 0; i < MAXR; i++) {
        buf_init(&s->raw[i]);
        buf_init(&s->plain[i]);
        s->seen[i] = 0;
    }
    s->ok = 1;
}

static void scr_free(Scr *s) {
    for (int i = 0; i < MAXR; i++) {
        buf_free(&s->raw[i]);
        buf_free(&s->plain[i]);
    }
}

static void scr_parse(Scr *s, const Buf *f) {
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
                if (row < 1 || row > MAXR) { s->ok = 0; return; }
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

/* ---- fixtures ---- */

typedef struct {
    Patch p;
    Model m;
    char *buf;
    int built;
} Rx;

static int rx_open(Rx *x, const char *s, size_t n, size_t idx) {
    ParseError err;
    x->buf = xmalloc(n ? n : 1);
    if (n) memcpy(x->buf, s, n);
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

static const char TWO[] =
    "diff --git a/src/a.c b/src/a.c\n"
    "--- a/src/a.c\n"
    "+++ b/src/a.c\n"
    "@@ -8,3 +8,3 @@ int main\n"
    " keep\n"
    "-old line\n"
    "+new line\n"
    " tail\n"
    "diff --git a/b.txt b/b.txt\n"
    "--- a/b.txt\n"
    "+++ b/b.txt\n"
    "@@ -1 +1 @@\n"
    "-x\n"
    "+y\n";

static void draw(const Rx *x, size_t idx, size_t count, size_t top, size_t left, int rows, int cols,
                 bool tc, Scr *s) {
    RenderView v;
    memset(&v, 0, sizeof v);
    v.model = &x->m;
    v.file_idx = idx;
    v.file_count = count;
    v.top = top;
    v.left = left;
    v.rows = rows;
    v.cols = cols;
    v.truecolor = tc;
    Buf f;
    buf_init(&f);
    render_frame(&f, &v);
    scr_init(s);
    scr_parse(s, &f);
    buf_free(&f);
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

/* Every painted row is exactly `cols` columns wide; every screen row is
 * painted. */
static int geometry_ok(const Scr *s, int rows, int cols) {
    if (!s->ok) return 0;
    for (int r = 1; r <= rows; r++) {
        if (!s->seen[r - 1]) return 0;
        const Buf *p = &s->plain[r - 1];
        if (p->len == 0) continue; /* erased body row */
        if (text_width(p->data, p->len) != (size_t)cols) return 0;
    }
    return 1;
}

/* ---- tests ---- */

static void test_helpers(void) {
    CHECK(render_body_rows(24) == 22);
    CHECK(render_body_rows(3) == 1);
    CHECK(render_body_rows(2) == 0);
    CHECK(render_body_rows(1) == 0);
    CHECK(render_body_rows(0) == 0);
    CHECK(render_body_rows(-5) == 0);

    Rx x;
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 0)); /* 6 rows: wave, hunk + 4 lines */
    CHECK(x.m.rows.len == 6);
    CHECK(render_max_top(&x.m, 24) == 0);
    CHECK(render_max_top(&x.m, 5) == 3);  /* body 3 */
    CHECK(render_max_top(&x.m, 3) == 5);  /* body 1 */
    CHECK(render_max_top(&x.m, 1) == 6);  /* body 0 */
    rx_close(&x);
}

static void test_layout(void) {
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 0));
    draw(&x, 0, 2, 0, 0, 12, 60, true, &s);
    CHECK(geometry_ok(&s, 12, 60));

    /* header */
    CHECK(starts(&s, 1, " 1/2 files   M  src/a.c"));
    CHECK(ends(&s, 1, "  +1 -1 "));
    CHECK(raw_has(&s, 1, "48;2;60;63;65"));

    /* the wave above the hunk spans the whole width, then the hunk row, then
     * gutters: widths are 2 and 2 for line numbers up to 10 */
    CHECK(starts(&s, 2, "~~~~"));
    CHECK(s.plain[1].len == 60);
    for (size_t i = 0; i < s.plain[1].len; i++) CHECK(s.plain[1].data[i] == '~');
    CHECK(starts(&s, 3, " @@ -8,3 +8,3 @@ int main"));
    CHECK(starts(&s, 4, "  8  8 keep"));
    CHECK(starts(&s, 5, "  9    old line"));
    CHECK(starts(&s, 6, "     9 new line"));
    CHECK(starts(&s, 7, " 10 10 tail"));

    /* rows past the content are erased */
    CHECK(s.plain[7].len == 0 && raw_has(&s, 8, "\x1b[2K"));
    CHECK(s.plain[10].len == 0);

    /* bottom bar */
    CHECK(starts(&s, 12, " q quit"));
    CHECK(has(&s, 12, "READ-ONLY"));
    CHECK(ends(&s, 12, " READ-ONLY  100% "));
    CHECK(raw_has(&s, 12, "\x1b[0;7m"));
    scr_free(&s);

    /* second file in the header */
    rx_close(&x);
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 1));
    draw(&x, 1, 2, 0, 0, 10, 40, true, &s);
    CHECK(starts(&s, 1, " 2/2 files   M  b.txt"));
    CHECK(ends(&s, 1, "  +1 -1 "));
    CHECK(starts(&s, 2, "~~~~"));
    CHECK(starts(&s, 4, " 1   x"));
    CHECK(starts(&s, 5, "   1 y"));
    CHECK(geometry_ok(&s, 10, 40));
    scr_free(&s);
    rx_close(&x);
}

static void test_colors(void) {
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 0));

    draw(&x, 0, 2, 0, 0, 10, 50, true, &s);
    CHECK(raw_has(&s, 5, "48;2;82;38;42"));  /* removed: red */
    CHECK(!raw_has(&s, 5, "48;2;36;68;46"));
    CHECK(raw_has(&s, 6, "48;2;36;68;46"));  /* added: green */
    CHECK(!raw_has(&s, 6, "48;2;82;38;42"));
    CHECK(raw_has(&s, 3, "48;2;43;48;66"));  /* hunk header */
    CHECK(raw_has(&s, 2, "38;2;110;110;110")); /* wave: gray text */
    CHECK(!strstr(s.raw[1].data, "48;"));    /* ... and no background */
    CHECK(!strstr(s.raw[3].data, "48;")); /* context row: no background */
    CHECK(!strstr(s.raw[6].data, "48;"));
    scr_free(&s);

    draw(&x, 0, 2, 0, 0, 10, 50, false, &s);
    CHECK(raw_has(&s, 5, "48;5;52"));
    CHECK(raw_has(&s, 6, "48;5;22"));
    CHECK(raw_has(&s, 3, "48;5;236"));
    CHECK(raw_has(&s, 2, "38;5;242"));
    CHECK(!strstr(s.raw[3].data, "38;2;"));
    CHECK(!strstr(s.raw[1].data, "38;2;"));
    scr_free(&s);
    rx_close(&x);
}

static void test_scroll(void) {
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 0));

    /* top = 3: wave, hunk row and first context row are scrolled off */
    draw(&x, 0, 2, 3, 0, 8, 50, true, &s);
    CHECK(starts(&s, 2, "  9    old line"));
    CHECK(starts(&s, 3, "     9 new line"));
    CHECK(starts(&s, 4, " 10 10 tail"));
    CHECK(s.plain[4].len == 0);
    CHECK(ends(&s, 8, "  100% "));
    scr_free(&s);

    /* partial view: 3 of 6 rows visible -> 50% */
    draw(&x, 0, 2, 0, 0, 5, 50, true, &s);
    CHECK(ends(&s, 5, " READ-ONLY  50% "));
    CHECK(geometry_ok(&s, 5, 50));
    scr_free(&s);

    /* a top far past the end must not crash and shows only blanks */
    draw(&x, 0, 2, 1000, 0, 8, 50, true, &s);
    CHECK(geometry_ok(&s, 8, 50));
    for (int r = 2; r <= 7; r++) CHECK(s.plain[r - 1].len == 0);
    scr_free(&s);

    /* top near SIZE_MAX: the index addition must not wrap */
    draw(&x, 0, 2, SIZE_MAX, 0, 8, 50, true, &s);
    CHECK(geometry_ok(&s, 8, 50));
    for (int r = 2; r <= 7; r++) CHECK(s.plain[r - 1].len == 0);
    scr_free(&s);
    draw(&x, 0, 2, SIZE_MAX - 1, 0, 8, 50, true, &s);
    CHECK(geometry_ok(&s, 8, 50));
    scr_free(&s);
    rx_close(&x);
}

static void test_horizontal(void) {
    static const char P[] =
        "--- a/f\n+++ b/f\n@@ -1 +1 @@\n-0123456789abcdef\n+0123456789ABCDEF\n";
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, P, sizeof P - 1, 0));

    draw(&x, 0, 1, 0, 0, 6, 16, true, &s);
    CHECK(starts(&s, 4, " 1   0123456789a")); /* gutter is 5 columns, 11 left for text */
    CHECK(geometry_ok(&s, 6, 16));
    scr_free(&s);

    draw(&x, 0, 1, 0, 10, 6, 16, true, &s);
    CHECK(starts(&s, 4, " 1   abcdef"));
    CHECK(starts(&s, 5, "   1 ABCDEF"));
    CHECK(geometry_ok(&s, 6, 16));
    scr_free(&s);

    /* scrolled past the end of the line: gutter stays, text is empty */
    draw(&x, 0, 1, 0, 500, 6, 16, true, &s);
    CHECK(starts(&s, 4, " 1   "));
    CHECK(geometry_ok(&s, 6, 16));
    scr_free(&s);

    /* the wave and hunk rows ignore horizontal scrolling */
    draw(&x, 0, 1, 0, 3, 6, 30, true, &s);
    CHECK(starts(&s, 2, "~~~~"));
    CHECK(starts(&s, 3, " @@ -1,1 +1,1 @@"));
    scr_free(&s);
    rx_close(&x);
}

static void test_content_safety(void) {
    static const char P[] =
        "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n"
        " \x1b[31mred\x1b[0m\ttab\n"
        "-old \x07\x01\n"
        "+\xe4\xb8\xad\xe6\x96\x87 wide \xff\xfe\n"
        "\\ No newline at end of file\n";
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, P, sizeof P - 1, 0));
    draw(&x, 0, 1, 0, 0, 10, 40, true, &s);
    CHECK(s.ok); /* no escape sequence other than our own */
    CHECK(geometry_ok(&s, 10, 40));
    CHECK(has(&s, 4, "\xe2\x90\x9b[31mred")); /* ESC shown as U+241B */
    CHECK(has(&s, 4, "    tab"));              /* tab expanded */
    CHECK(has(&s, 5, "old \xe2\x90\x87"));    /* BEL shown as U+2407 */
    CHECK(has(&s, 6, "\xe4\xb8\xad\xe6\x96\x87 wide \xef\xbf\xbd\xef\xbf\xbd"));
    CHECK(ends(&s, 6, "  [no newline]") == 0); /* padded after the marker */
    CHECK(has(&s, 6, "  [no newline]"));
    scr_free(&s);

    /* a wide character cut by the text edge shows as a space, width intact */
    for (int cols = 1; cols <= 20; cols++) {
        for (size_t left = 0; left < 4; left++) {
            draw(&x, 0, 1, 0, left, 10, cols, true, &s);
            CHECK(geometry_ok(&s, 10, cols));
            scr_free(&s);
        }
    }
    rx_close(&x);

    /* hostile path names cannot inject escapes into the header */
    static const char Q[] = "--- a/\x1b[2Jbad\n+++ b/\x1b[2Jbad\n@@ -1 +1 @@\n-a\n+b\n";
    REQUIRE(rx_open(&x, Q, sizeof Q - 1, 0));
    draw(&x, 0, 1, 0, 0, 6, 40, true, &s);
    CHECK(s.ok);
    CHECK(geometry_ok(&s, 6, 40));
    scr_free(&s);
    rx_close(&x);
}

static void test_special_files(void) {
    static const char BIN[] =
        "diff --git a/img.png b/img.png\nnew file mode 100644\nBinary files /dev/null and b/img.png differ\n";
    static const char REN[] =
        "diff --git a/old.c b/new.c\nsimilarity index 100%\nrename from old.c\nrename to new.c\n";
    Rx x;
    Scr s;

    REQUIRE(rx_open(&x, BIN, sizeof BIN - 1, 0));
    draw(&x, 0, 1, 0, 0, 6, 40, true, &s);
    CHECK(starts(&s, 1, " 1/1 files   A  img.png"));
    CHECK(starts(&s, 2, " Binary file not shown"));
    CHECK(raw_has(&s, 2, "\x1b[0;2m")); /* dim note */
    CHECK(geometry_ok(&s, 6, 40));
    scr_free(&s);
    rx_close(&x);

    REQUIRE(rx_open(&x, REN, sizeof REN - 1, 0));
    draw(&x, 0, 1, 0, 0, 6, 40, true, &s);
    CHECK(starts(&s, 1, " 1/1 files   R  new.c"));
    CHECK(starts(&s, 2, " Renamed from old.c"));
    CHECK(geometry_ok(&s, 6, 40));
    scr_free(&s);
    rx_close(&x);
}

static void test_tiny_screens(void) {
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, TWO, sizeof TWO - 1, 0));

    for (int rows = 1; rows <= 6; rows++) {
        for (int cols = 1; cols <= 30; cols++) {
            draw(&x, 0, 2, 0, 0, rows, cols, rows % 2, &s);
            CHECK(geometry_ok(&s, rows, cols));
            scr_free(&s);
        }
    }

    /* nothing at all for an empty screen */
    RenderView v;
    memset(&v, 0, sizeof v);
    v.model = &x.m;
    v.file_count = 1;
    Buf f;
    buf_init(&f);
    for (int k = 0; k < 4; k++) {
        v.rows = k == 0 ? 0 : k == 1 ? -3 : 10;
        v.cols = k == 2 ? 0 : k == 3 ? -1 : 10;
        if (k == 0 || k == 1) v.cols = 10;
        render_frame(&f, &v);
        CHECK(f.len == 0);
    }
    buf_free(&f);
    rx_close(&x);
}

static void test_wide_line_numbers(void) {
    static const char P[] =
        "--- a/f\n+++ b/f\n@@ -99998,2 +99998,3 @@\n a\n+b\n c\n";
    Rx x;
    Scr s;
    REQUIRE(rx_open(&x, P, sizeof P - 1, 0));
    draw(&x, 0, 1, 0, 0, 8, 40, true, &s);
    CHECK(starts(&s, 4, " 99998  99998 a"));
    CHECK(starts(&s, 5, "        99999 b"));
    CHECK(starts(&s, 6, " 99999 100000 c"));
    CHECK(geometry_ok(&s, 8, 40));
    scr_free(&s);
    rx_close(&x);
}

static void test_render_fuzz(void) {
    Buf m;
    buf_init(&m);
    int rendered = 0;
    for (int iter = 0; iter < 3000; iter++) {
        mutate(&m, TWO, sizeof TWO - 1);
        char *copy = xmalloc(m.len ? m.len : 1);
        if (m.len) memcpy(copy, m.data, m.len);
        Patch p;
        ParseError err;
        int rc = patch_parse(&p, copy, m.len, &err);
        if (rc == 0) {
            for (size_t i = 0; i < p.files.len; i++) {
                Model mod;
                model_build(&mod, &p, vec_at(&p.files, i));
                RenderView v;
                memset(&v, 0, sizeof v);
                v.model = &mod;
                v.file_idx = i;
                v.file_count = p.files.len;
                v.rows = (int)(1 + rnd() % 30);
                v.cols = (int)(1 + rnd() % 90);
                v.top = rnd() % (mod.rows.len + 3);
                v.left = rnd() % 30;
                v.truecolor = rnd() & 1;
                Buf f;
                buf_init(&f);
                render_frame(&f, &v);
                Scr s;
                scr_init(&s);
                scr_parse(&s, &f);
                CHECK(geometry_ok(&s, v.rows, v.cols));
                rendered++;
                scr_free(&s);
                buf_free(&f);
                model_free(&mod);
            }
        }
        patch_free(&p);
        free(copy);
    }
    CHECK(rendered > 500); /* the mutator leaves enough parseable inputs */
    buf_free(&m);
}

int main(void) {
    printf("test_render\n");
    test_helpers();
    test_layout();
    test_colors();
    test_scroll();
    test_horizontal();
    test_content_safety();
    test_special_files();
    test_tiny_screens();
    test_wide_line_numbers();
    test_render_fuzz();
    TEST_DONE();
}
