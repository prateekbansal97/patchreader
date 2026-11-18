#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "model.h"
#include "patch.h"
#include "util.h"

#define REQUIRE(cond)        \
    do {                     \
        CHECK(cond);         \
        if (!(cond)) return; \
    } while (0)

/* Parses s (exact-size heap copy, so ASan flags overreads) and builds the
 * model of file `idx`. */
typedef struct {
    Patch p;
    Model m;
    char *buf;
    size_t n;
    int built;
} Mx;

static int mx_open_n(Mx *x, const char *s, size_t n, size_t idx) {
    ParseError err;
    x->buf = xmalloc(n);
    if (n) memcpy(x->buf, s, n);
    x->n = n;
    x->built = 0;
    int rc = patch_parse(&x->p, x->buf, x->n, &err);
    if (rc == 0 && idx < x->p.files.len) {
        model_build(&x->m, &x->p, vec_at(&x->p.files, idx));
        x->built = 1;
    }
    CHECK(x->built);
    return x->built;
}

static int mx_open(Mx *x, const char *s, size_t idx) {
    return mx_open_n(x, s, strlen(s), idx);
}

static void mx_close(Mx *x) {
    if (x->built) model_free(&x->m);
    patch_free(&x->p);
    free(x->buf);
    x->buf = NULL;
}

static int row_is(const Mx *x, size_t i, RowKind k, size_t o, size_t n, const char *text) {
    if (i >= x->m.rows.len) return 0;
    const Row *r = vec_at(&x->m.rows, i);
    size_t tl = strlen(text);
    return r->kind == k && r->old_no == o && r->new_no == n && r->len == tl &&
           memcmp(r->text, text, tl) == 0;
}

#define ROW(x, i, kind, o, n, text) CHECK(row_is(&(x), (i), (kind), (o), (n), (text)))

/* ---- num_digits ---- */

static void test_num_digits(void) {
    CHECK(num_digits(0) == 1);
    CHECK(num_digits(9) == 1);
    CHECK(num_digits(10) == 2);
    CHECK(num_digits(99) == 2);
    CHECK(num_digits(100) == 3);
    CHECK(num_digits(999999999) == 9);
    CHECK(num_digits(SIZE_MAX) == (SIZE_MAX > 0xffffffffu ? 20u : 10u));
}

/* ---- line numbers and rows ---- */

static void test_numbering(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "diff --git a/f b/f\n"
                    "--- a/f\n"
                    "+++ b/f\n"
                    "@@ -8,3 +98,4 @@ fn x\n"
                    " ctx8\n"
                    "-del9\n"
                    "+add99\n"
                    "+add100\n"
                    " ctx10\n"
                    "@@ -50,0 +150,2 @@ other\n"
                    "+n1\n"
                    "+n2\n",
                    0));
    REQUIRE(x.m.rows.len == 9);
    ROW(x, 0, ROW_HUNK, 0, 0, "@@ -8,3 +98,4 @@ fn x");
    ROW(x, 1, ROW_CONTEXT, 8, 98, "ctx8");
    ROW(x, 2, ROW_DEL, 9, 0, "del9");
    ROW(x, 3, ROW_ADD, 0, 99, "add99");
    ROW(x, 4, ROW_ADD, 0, 100, "add100");
    ROW(x, 5, ROW_CONTEXT, 10, 101, "ctx10");
    ROW(x, 6, ROW_HUNK, 0, 0, "@@ -50,0 +150,2 @@ other");
    ROW(x, 7, ROW_ADD, 0, 150, "n1");
    ROW(x, 8, ROW_ADD, 0, 151, "n2");
    CHECK(x.m.old_w == 2); /* widest old number is 10 */
    CHECK(x.m.new_w == 3); /* widest new number is 151 */
    CHECK(x.m.max_cols == 6);
    CHECK(x.m.file == vec_at(&x.p.files, 0));
    mx_close(&x);
}

static void test_hunk_header_text(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "--- a\n"
                    "+++ b\n"
                    "@@ -3 +3 @@\n"
                    "-x\n"
                    "+y\n",
                    0));
    /* omitted counts are shown as 1; no heading means no trailing space */
    ROW(x, 0, ROW_HUNK, 0, 0, "@@ -3,1 +3,1 @@");
    mx_close(&x);
}

static void test_added_and_deleted(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "diff --git a/n b/n\n"
                    "new file mode 100644\n"
                    "--- /dev/null\n"
                    "+++ b/n\n"
                    "@@ -0,0 +1,12 @@\n"
                    "+a\n+b\n+c\n+d\n+e\n+f\n+g\n+h\n+i\n+j\n+k\n+l\n",
                    0));
    REQUIRE(x.m.rows.len == 13);
    ROW(x, 1, ROW_ADD, 0, 1, "a");
    ROW(x, 12, ROW_ADD, 0, 12, "l");
    CHECK(x.m.old_w == 1 && x.m.new_w == 2);
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/g b/g\n"
                    "deleted file mode 100644\n"
                    "--- a/g\n"
                    "+++ /dev/null\n"
                    "@@ -1,2 +0,0 @@\n"
                    "-one\n-two\n",
                    0));
    REQUIRE(x.m.rows.len == 3);
    ROW(x, 1, ROW_DEL, 1, 0, "one");
    ROW(x, 2, ROW_DEL, 2, 0, "two");
    CHECK(x.m.old_w == 1 && x.m.new_w == 1);
    mx_close(&x);
}

static void test_no_eol_and_empty_lines(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "--- a\n"
                    "+++ b\n"
                    "@@ -1,3 +1,3 @@\n"
                    " keep\n"
                    "\n"
                    "-old\n"
                    "\\ No newline at end of file\n"
                    "+new\n",
                    0));
    REQUIRE(x.m.rows.len == 5);
    ROW(x, 2, ROW_CONTEXT, 2, 2, ""); /* empty context line */
    const Row *del = vec_at(&x.m.rows, 3);
    const Row *add = vec_at(&x.m.rows, 4);
    CHECK(del->kind == ROW_DEL && del->no_eol);
    CHECK(add->kind == ROW_ADD && !add->no_eol);
    mx_close(&x);
}

static void test_max_cols(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "--- a\n"
                    "+++ b\n"
                    "@@ -0,0 +1,3 @@\n"
                    "+\tx\n"
                    "+\xE4\xB8\xAD\xE6\x96\x87\n"
                    "+ab\n",
                    0));
    CHECK(x.m.max_cols == 5); /* tab (4) + x */
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "--- a\n"
                    "+++ b\n"
                    "@@ -0,0 +1,1 @@\n"
                    "+\xE4\xB8\xAD\xE6\x96\x87\xE4\xB8\xAD\n",
                    0));
    CHECK(x.m.max_cols == 6); /* three wide characters */
    mx_close(&x);
}

/* ---- notes for files without hunks ---- */

static void test_notes(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "diff --git a/img.png b/img.png\n"
                    "Binary files a/img.png and b/img.png differ\n",
                    0));
    REQUIRE(x.m.rows.len == 1);
    ROW(x, 0, ROW_NOTE, 0, 0, "Binary file not shown");
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/p b/q\n"
                    "similarity index 100%\n"
                    "rename from p\n"
                    "rename to q\n",
                    0));
    REQUIRE(x.m.rows.len == 1);
    ROW(x, 0, ROW_NOTE, 0, 0, "Renamed from p");
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/c1 b/c2\n"
                    "copy from c1\n"
                    "copy to c2\n",
                    0));
    ROW(x, 0, ROW_NOTE, 0, 0, "Copied from c1");
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/run.sh b/run.sh\n"
                    "old mode 100644\n"
                    "new mode 100755\n",
                    0));
    ROW(x, 0, ROW_NOTE, 0, 0, "No content changes");
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/e b/e\n"
                    "new file mode 100644\n"
                    "index 0000000..e69de29\n",
                    0));
    ROW(x, 0, ROW_NOTE, 0, 0, "Empty file added");
    mx_close(&x);

    REQUIRE(mx_open(&x,
                    "diff --git a/e b/e\n"
                    "deleted file mode 100644\n"
                    "index e69de29..0000000\n",
                    0));
    ROW(x, 0, ROW_NOTE, 0, 0, "Empty file deleted");
    mx_close(&x);
}

/* ---- several files, ownership ---- */

static void test_multiple_files(void) {
    const char *patch =
        "diff --git a/a b/a\n--- a/a\n+++ b/a\n@@ -1 +1 @@\n-x\n+y\n"
        "diff --git a/b b/b\n--- a/b\n+++ b/b\n@@ -100,2 +100,2 @@\n k\n-l\n+m\n";
    Mx x0, x1;
    REQUIRE(mx_open(&x0, patch, 0));
    if (!mx_open(&x1, patch, 1)) {
        mx_close(&x0);
        return;
    }
    CHECK(x0.m.rows.len == 3 && x1.m.rows.len == 4);
    CHECK(x0.m.old_w == 1 && x1.m.old_w == 3);
    ROW(x1, 1, ROW_CONTEXT, 100, 100, "k");
    mx_close(&x1);
    mx_close(&x0);
}

static void test_rows_point_into_source(void) {
    Mx x;
    REQUIRE(mx_open(&x,
                    "--- a\n+++ b\n@@ -1,2 +1,2 @@\n a\n-b\n+c\n",
                    0));
    int inside = 1;
    for (size_t i = 0; i < x.m.rows.len; i++) {
        const Row *r = vec_at(&x.m.rows, i);
        if (r->kind == ROW_HUNK || r->kind == ROW_NOTE) continue;
        if (r->text < x.buf || r->text > x.buf + x.n ||
            r->len > (size_t)(x.buf + x.n - r->text))
            inside = 0;
    }
    CHECK(inside);
    mx_close(&x);
}

static void test_free(void) {
    Mx x;
    REQUIRE(mx_open(&x, "--- a\n+++ b\n@@ -1 +1 @@\n-a\n+b\n", 0));
    model_free(&x.m);
    CHECK(x.m.rows.len == 0 && x.m.rows.data == NULL && x.m.file == NULL);
    model_free(&x.m); /* second free is a no-op */
    x.built = 0;
    mx_close(&x);
}

/* ---- scale ---- */

static void test_large(void) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "--- a\n+++ b\n@@ -1,50000 +1,50000 @@\n");
    for (size_t i = 0; i < 50000; i++) buf_puts(&b, " line\n");
    Mx x;
    REQUIRE(mx_open_n(&x, b.data, b.len, 0));
    REQUIRE(x.m.rows.len == 50001);
    const Row *last = vec_at(&x.m.rows, 50000);
    CHECK(last->old_no == 50000 && last->new_no == 50000);
    CHECK(x.m.old_w == 5 && x.m.new_w == 5);
    mx_close(&x);

    buf_clear(&b);
    buf_puts(&b, "--- a\n+++ b\n@@ -0,0 +1 @@\n+");
    for (size_t i = 0; i < ((size_t)1 << 20); i++) buf_putc(&b, 'x');
    buf_putc(&b, '\n');
    REQUIRE(mx_open_n(&x, b.data, b.len, 0));
    CHECK(x.m.max_cols == ((size_t)1 << 20));
    mx_close(&x);
    buf_free(&b);
}

/* ---- corruption fuzz: whatever parses must also model cleanly ---- */

static const char SAMPLE[] =
    "diff --git a/a b/a\nindex 1..2 100644\n--- a/a\n+++ b/a\n"
    "@@ -1,4 +1,4 @@ fn\n one\n-two\n+TWO\n three\n four\n@@ -20 +20,2 @@\n-x\n+y\n+z\n"
    "\\ No newline at end of file\n"
    "diff --git a/n b/n\nnew file mode 100644\n--- /dev/null\n+++ b/n\n@@ -0,0 +1 @@\n+\xE4\xB8\xAD\thi\n"
    "diff --git a/o b/q\nrename from o\nrename to q\n"
    "diff --git a/m b/m\nold mode 100644\nnew mode 100755\n"
    "diff --git a/b b/b\nBinary files a/b and b/b differ\n"
    "--- plain\t2020\n+++ plain2\t2021\n@@ -1 +1 @@\n-1\n+2\n";

static int model_ok(const Mx *x) {
    const Model *m = &x->m;
    if (m->rows.len == 0) return 0;
    for (size_t i = 0; i < m->rows.len; i++) {
        const Row *r = vec_at(&m->rows, i);
        if (num_digits(r->old_no) > m->old_w || num_digits(r->new_no) > m->new_w) return 0;
        if (r->kind == ROW_HUNK || r->kind == ROW_NOTE) {
            if (r->text == NULL || r->text[r->len] != '\0') return 0; /* arena strings are NUL-terminated */
            continue;
        }
        if (r->text < x->buf || r->text > x->buf + x->n ||
            r->len > (size_t)(x->buf + x->n - r->text))
            return 0;
    }
    return 1;
}

static void test_fuzz(void) {
    Buf b;
    buf_init(&b);
    int bad = 0, models = 0;
    for (int it = 0; it < 5000; it++) {
        mutate(&b, SAMPLE, sizeof SAMPLE - 1);
        Mx x;
        x.buf = xmalloc(b.len);
        if (b.len) memcpy(x.buf, b.data, b.len);
        x.n = b.len;
        x.built = 0;
        if (patch_parse(&x.p, x.buf, x.n, NULL) == 0) {
            for (size_t f = 0; f < x.p.files.len; f++) {
                model_build(&x.m, &x.p, vec_at(&x.p.files, f));
                if (!model_ok(&x)) bad++;
                model_free(&x.m);
                models++;
            }
        }
        patch_free(&x.p);
        free(x.buf);
    }
    CHECK(bad == 0);
    CHECK(models > 1000);
    buf_free(&b);
}

int main(void) {
    printf("test_model\n");
    test_num_digits();
    test_numbering();
    test_hunk_header_text();
    test_added_and_deleted();
    test_no_eol_and_empty_lines();
    test_max_cols();
    test_notes();
    test_multiple_files();
    test_rows_point_into_source();
    test_free();
    test_large();
    test_fuzz();
    TEST_DONE();
}
