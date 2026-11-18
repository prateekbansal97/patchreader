#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "patch.h"
#include "util.h"

#define REQUIRE(cond)       \
    do {                    \
        CHECK(cond);        \
        if (!(cond)) return; \
    } while (0)

/* Fixture: the input lives in an exact-size heap copy with no trailing NUL,
 * so ASan catches any read past the end of the input. */
typedef struct {
    Patch p;
    char *buf;
    size_t n;
    ParseError err;
    int rc;
} Fx;

static void fx_parse_n(Fx *x, const char *s, size_t n) {
    x->buf = xmalloc(n);
    if (n) memcpy(x->buf, s, n);
    x->n = n;
    x->rc = patch_parse(&x->p, x->buf, x->n, &x->err);
}

static void fx_parse(Fx *x, const char *s) {
    fx_parse_n(x, s, strlen(s));
}

static void fx_free(Fx *x) {
    patch_free(&x->p);
    free(x->buf);
    x->buf = NULL;
}

static FileDiff *file_at(Fx *x, size_t i) { return vec_at(&x->p.files, i); }
static Hunk *hunk_at(FileDiff *f, size_t i) { return vec_at(&f->hunks, i); }
static Line *line_at_(Hunk *h, size_t i) { return vec_at(&h->lines, i); }

static int str_eq(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

static int line_is(const Fx *x, const Line *l, LineKind k, const char *text) {
    size_t n = strlen(text);
    return l->kind == k && l->len == n && l->off + l->len <= x->n &&
           memcmp(x->buf + l->off, text, n) == 0;
}

static int hdr_is(const Hunk *h, size_t os, size_t oc, size_t ns, size_t nc) {
    return h->old_start == os && h->old_count == oc && h->new_start == ns &&
           h->new_count == nc;
}

/* ---- basic structure ---- */

static void test_basic(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/foo.c b/foo.c\n"
             "index 111..222 100644\n"
             "--- a/foo.c\n"
             "+++ b/foo.c\n"
             "@@ -1,4 +1,4 @@ int main(void)\n"
             " one\n"
             "-two\n"
             "+TWO\n"
             " three\n"
             " four\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    FileDiff *f = file_at(&x, 0);
    CHECK(f->status == DIFF_MODIFIED);
    CHECK(str_eq(f->old_path, "foo.c") && str_eq(f->new_path, "foo.c"));
    CHECK(str_eq(filediff_path(f), "foo.c"));
    CHECK(f->adds == 1 && f->dels == 1 && !f->is_binary);
    REQUIRE(f->hunks.len == 1);
    Hunk *h = hunk_at(f, 0);
    CHECK(hdr_is(h, 1, 4, 1, 4));
    CHECK(h->head_len == 14 && memcmp(x.buf + h->head_off, "int main(void)", 14) == 0);
    REQUIRE(h->lines.len == 5);
    CHECK(line_is(&x, line_at_(h, 0), LINE_CONTEXT, "one"));
    CHECK(line_is(&x, line_at_(h, 1), LINE_DEL, "two"));
    CHECK(line_is(&x, line_at_(h, 2), LINE_ADD, "TWO"));
    CHECK(line_is(&x, line_at_(h, 3), LINE_CONTEXT, "three"));
    CHECK(line_is(&x, line_at_(h, 4), LINE_CONTEXT, "four"));
    fx_free(&x);
}

static void test_multi_file_multi_hunk(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/a.txt b/a.txt\n"
             "--- a/a.txt\n"
             "+++ b/a.txt\n"
             "@@ -1,2 +1,3 @@\n"
             " x\n"
             "+y\n"
             " z\n"
             "@@ -10 +11,2 @@ func\n"
             "-old\n"
             "+new1\n"
             "+new2\n"
             "diff --git a/b.txt b/b.txt\n"
             "--- a/b.txt\n"
             "+++ b/b.txt\n"
             "@@ -1 +1 @@\n"
             "-p\n"
             "+q\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 2);
    FileDiff *a = file_at(&x, 0);
    FileDiff *b = file_at(&x, 1);
    CHECK(str_eq(filediff_path(a), "a.txt") && str_eq(filediff_path(b), "b.txt"));
    CHECK(a->adds == 3 && a->dels == 1 && b->adds == 1 && b->dels == 1);
    REQUIRE(a->hunks.len == 2 && b->hunks.len == 1);
    CHECK(hdr_is(hunk_at(a, 0), 1, 2, 1, 3));
    CHECK(hdr_is(hunk_at(a, 1), 10, 1, 11, 2)); /* omitted old count means 1 */
    CHECK(hunk_at(a, 0)->lines.len == 3 && hunk_at(a, 1)->lines.len == 3);
    CHECK(hdr_is(hunk_at(b, 0), 1, 1, 1, 1));
    fx_free(&x);
}

/* ---- added / deleted / renamed / mode / binary ---- */

static void test_added_deleted(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/new.txt b/new.txt\n"
             "new file mode 100644\n"
             "index 0000000..e69de29\n"
             "--- /dev/null\n"
             "+++ b/new.txt\n"
             "@@ -0,0 +1,2 @@\n"
             "+l1\n"
             "+l2\n"
             "diff --git a/gone.txt b/gone.txt\n"
             "deleted file mode 100644\n"
             "index e69de29..0000000\n"
             "--- a/gone.txt\n"
             "+++ /dev/null\n"
             "@@ -1 +0,0 @@\n"
             "-bye\n"
             "diff --git a/empty b/empty\n"
             "new file mode 100644\n"
             "index 0000000..e69de29\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 3);
    FileDiff *a = file_at(&x, 0);
    FileDiff *d = file_at(&x, 1);
    FileDiff *e = file_at(&x, 2);
    CHECK(a->status == DIFF_ADDED && a->old_path == NULL && str_eq(a->new_path, "new.txt"));
    CHECK(str_eq(filediff_path(a), "new.txt"));
    REQUIRE(a->hunks.len == 1);
    CHECK(hdr_is(hunk_at(a, 0), 0, 0, 1, 2));
    CHECK(d->status == DIFF_DELETED && d->new_path == NULL && str_eq(d->old_path, "gone.txt"));
    CHECK(str_eq(filediff_path(d), "gone.txt"));
    CHECK(d->dels == 1 && d->adds == 0);
    CHECK(e->status == DIFF_ADDED && e->hunks.len == 0 && str_eq(filediff_path(e), "empty"));
    fx_free(&x);
}

static void test_rename_copy_mode(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/old name.txt b/new name.txt\n"
             "similarity index 90%\n"
             "rename from old name.txt\n"
             "rename to new name.txt\n"
             "index 1..2 100644\n"
             "--- a/old name.txt\n"
             "+++ b/new name.txt\n"
             "@@ -1 +1,2 @@\n"
             " x\n"
             "+y\n"
             "diff --git a/p b/q\n"
             "similarity index 100%\n"
             "rename from p\n"
             "rename to q\n"
             "diff --git a/c1 b/c2\n"
             "copy from c1\n"
             "copy to c2\n"
             "diff --git a/run.sh b/run.sh\n"
             "old mode 100644\n"
             "new mode 100755\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 4);
    FileDiff *r1 = file_at(&x, 0);
    FileDiff *r2 = file_at(&x, 1);
    FileDiff *cp = file_at(&x, 2);
    FileDiff *md = file_at(&x, 3);
    CHECK(r1->status == DIFF_RENAMED && str_eq(r1->old_path, "old name.txt") &&
          str_eq(r1->new_path, "new name.txt") && r1->hunks.len == 1);
    CHECK(r2->status == DIFF_RENAMED && str_eq(r2->old_path, "p") && str_eq(r2->new_path, "q") &&
          r2->hunks.len == 0);
    CHECK(cp->status == DIFF_COPIED && str_eq(cp->old_path, "c1") && str_eq(cp->new_path, "c2"));
    CHECK(md->status == DIFF_MODIFIED && str_eq(filediff_path(md), "run.sh") && md->hunks.len == 0);
    fx_free(&x);
}

static void test_binary(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/img.png b/img.png\n"
             "index 1..2 100644\n"
             "Binary files a/img.png and b/img.png differ\n"
             "diff --git a/x b/x\n"
             "index 1..2 100644\n"
             "GIT binary patch\n"
             "literal 12\n"
             "zc${ABC\n"
             "\n"
             "literal 0\n"
             "HcmV?d00001\n"
             "\n"
             "diff --git a/t.txt b/t.txt\n"
             "--- a/t.txt\n"
             "+++ b/t.txt\n"
             "@@ -1 +1 @@\n"
             "-a\n"
             "+b\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 3);
    CHECK(file_at(&x, 0)->is_binary && file_at(&x, 0)->hunks.len == 0);
    CHECK(file_at(&x, 1)->is_binary && file_at(&x, 1)->hunks.len == 0);
    CHECK(!file_at(&x, 2)->is_binary && file_at(&x, 2)->hunks.len == 1);
    CHECK(str_eq(filediff_path(file_at(&x, 0)), "img.png"));

    /* plain diff -r style */
    Fx y;
    fx_parse(&y, "Binary files o/bin and n/bin differ\n");
    REQUIRE(y.rc == 0 && y.p.files.len == 1);
    CHECK(file_at(&y, 0)->is_binary && str_eq(filediff_path(file_at(&y, 0)), "n/bin"));
    fx_free(&y);
    fx_free(&x);
}

/* ---- plain unified diff ---- */

static void test_plain_diff(void) {
    Fx x;
    fx_parse(&x,
             "diff -u a/old.txt b/old.txt\n"
             "--- a/old.txt\t2020-01-01 10:00:00.000000000 +0000\n"
             "+++ b/old.txt\t2020-01-02 10:00:00.000000000 +0000\n"
             "@@ -1,2 +1,2 @@\n"
             " keep\n"
             "-x\n"
             "+y\n"
             "--- /dev/null\n"
             "+++ added.txt\n"
             "@@ -0,0 +1 @@\n"
             "+hello\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 2);
    FileDiff *a = file_at(&x, 0);
    FileDiff *b = file_at(&x, 1);
    /* plain diffs keep their names verbatim (only git paths lose a/ and b/) */
    CHECK(a->status == DIFF_MODIFIED && str_eq(a->old_path, "a/old.txt") &&
          str_eq(a->new_path, "b/old.txt"));
    CHECK(b->status == DIFF_ADDED && b->old_path == NULL && str_eq(b->new_path, "added.txt"));
    CHECK(hunk_at(a, 0)->lines.len == 3 && hunk_at(b, 0)->lines.len == 1);
    fx_free(&x);
}

/* ---- line details ---- */

static void test_no_newline_markers(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/n b/n\n"
             "--- a/n\n"
             "+++ b/n\n"
             "@@ -1 +1 @@\n"
             "-a\n"
             "\\ No newline at end of file\n"
             "+b\n"
             "\\ No newline at end of file\n"
             "diff --git a/m b/m\n"
             "--- a/m\n"
             "+++ b/m\n"
             "@@ -1 +1 @@\n"
             "-a\n"
             "\\ No newline at end of file\n"
             "+b\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 2);
    Hunk *h1 = hunk_at(file_at(&x, 0), 0);
    Hunk *h2 = hunk_at(file_at(&x, 1), 0);
    REQUIRE(h1->lines.len == 2 && h2->lines.len == 2);
    CHECK(line_at_(h1, 0)->no_eol && line_at_(h1, 1)->no_eol);
    CHECK(line_at_(h2, 0)->no_eol && !line_at_(h2, 1)->no_eol);
    CHECK(line_is(&x, line_at_(h1, 1), LINE_ADD, "b"));
    fx_free(&x);
}

static void test_crlf(void) {
    Fx x;
    fx_parse(&x,
             "diff --git a/c b/c\r\n"
             "--- a/c\r\n"
             "+++ b/c\r\n"
             "@@ -1 +1 @@ head\r\n"
             "-a\r\n"
             "+b\r\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    FileDiff *f = file_at(&x, 0);
    CHECK(str_eq(f->new_path, "c"));
    Hunk *h = hunk_at(f, 0);
    CHECK(h->head_len == 4);
    REQUIRE(h->lines.len == 2);
    CHECK(line_is(&x, line_at_(h, 0), LINE_DEL, "a"));
    CHECK(line_is(&x, line_at_(h, 1), LINE_ADD, "b"));
    fx_free(&x);
}

static void test_stripped_empty_context(void) {
    Fx x;
    fx_parse(&x,
             "--- a\n"
             "+++ b\n"
             "@@ -1,3 +1,3 @@\n"
             " x\n"
             "\n"
             "-y\n"
             "+z\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    Hunk *h = hunk_at(file_at(&x, 0), 0);
    REQUIRE(h->lines.len == 4);
    CHECK(line_is(&x, line_at_(h, 1), LINE_CONTEXT, ""));
    fx_free(&x);
}

static void test_hunk_body_looks_like_headers(void) {
    /* deleted "-- foo" and added "++ bar" look like "--- foo" / "+++ bar" */
    Fx x;
    fx_parse(&x,
             "diff --git a/f b/f\n"
             "--- a/f\n"
             "+++ b/f\n"
             "@@ -1 +1 @@\n"
             "--- foo\n"
             "+++ bar\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    FileDiff *f = file_at(&x, 0);
    CHECK(f->dels == 1 && f->adds == 1);
    Hunk *h = hunk_at(f, 0);
    REQUIRE(h->lines.len == 2);
    CHECK(line_is(&x, line_at_(h, 0), LINE_DEL, "-- foo"));
    CHECK(line_is(&x, line_at_(h, 1), LINE_ADD, "++ bar"));
    fx_free(&x);

    /* "diff --git" inside a hunk body is content, not a new file */
    fx_parse(&x,
             "diff --git a/f b/f\n"
             "--- a/f\n"
             "+++ b/f\n"
             "@@ -1 +1 @@\n"
             "-diff --git a/x b/x\n"
             "+ok\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    fx_free(&x);
}

/* ---- surrounding text ---- */

static void test_mail_preamble(void) {
    Fx x;
    fx_parse(&x,
             "From 1234 Mon Sep 17 00:00:00 2001\n"
             "From: A <a@b>\n"
             "Subject: [PATCH] fix thing\n"
             "\n"
             "Body quoting a diff:\n"
             "--- not a header\n"
             "+++ nor this\n"
             "\n"
             "---\n"
             " f.txt | 2 +-\n"
             " 1 file changed, 1 insertion(+), 1 deletion(-)\n"
             "\n"
             "diff --git a/f.txt b/f.txt\n"
             "index 1..2 100644\n"
             "--- a/f.txt\n"
             "+++ b/f.txt\n"
             "@@ -1 +1 @@\n"
             "-old\n"
             "+new\n"
             "-- \n"
             "2.39.0\n"
             "\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    FileDiff *f = file_at(&x, 0);
    CHECK(str_eq(filediff_path(f), "f.txt") && f->adds == 1 && f->dels == 1);
    CHECK(hunk_at(f, 0)->lines.len == 2);
    fx_free(&x);
}

static void test_quoted_and_spaced_paths(void) {
    Fx x;
    fx_parse(&x,
             "diff --git \"a/we\\tird.txt\" \"b/we\\tird.txt\"\n"
             "--- \"a/we\\tird.txt\"\n"
             "+++ \"b/we\\tird.txt\"\n"
             "@@ -1 +1 @@\n"
             "-a\n"
             "+b\n"
             "diff --git \"a/uni \\303\\251.txt\" \"b/uni \\303\\251.txt\"\n"
             "old mode 100644\n"
             "new mode 100755\n"
             "diff --git a/a b.txt b/a b.txt\n"
             "old mode 100644\n"
             "new mode 100755\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 3);
    CHECK(str_eq(filediff_path(file_at(&x, 0)), "we\tird.txt"));
    CHECK(str_eq(filediff_path(file_at(&x, 1)), "uni \xc3\xa9.txt"));
    CHECK(str_eq(filediff_path(file_at(&x, 2)), "a b.txt"));
    fx_free(&x);
}

/* ---- errors ---- */

static void expect_err(const char *input, size_t line, const char *msg_part) {
    Fx x;
    fx_parse(&x, input);
    CHECK(x.rc == -1);
    CHECK(x.err.line == line);
    CHECK(strstr(x.err.msg, msg_part) != NULL);
    if (x.err.line != line || strstr(x.err.msg, msg_part) == NULL)
        fprintf(stderr, "    input: %.40s... got line %zu: %s\n", input, x.err.line, x.err.msg);
    fx_free(&x); /* must be safe after a failed parse (leak check by ASan) */
}

static void test_errors(void) {
    expect_err("hello\nworld\n", 0, "no diff");
    expect_err("", 0, "no diff");
    expect_err("@@ -1 +1 @@\n-a\n+b\n", 0, "no diff"); /* hunk without a file */
    expect_err("--- a\n+++ b\n@@ -x +1 @@\n", 3, "malformed");
    expect_err("--- a\n+++ b\n@@ -1,2147483648 +1 @@\n", 3, "malformed");
    expect_err("--- a\n+++ b\n@@ -1,99999999999999999999 +1 @@\n", 3, "malformed");
    expect_err("--- a\n+++ b\n@@ -1 +1 @@\n-a\n", 5, "unexpected end");
    expect_err("--- a\n+++ b\n@@ -1,2147483647 +1,2147483647 @@\n a\n", 5, "unexpected end");
    expect_err("--- a\n+++ b\n@@ -1,2 +1,2 @@\n a\nzzz\n", 5, "corrupt");
    expect_err("--- a\n+++ b\n@@ -1 +1 @@\n-a\n-b\n+c\n", 5, "corrupt");
    expect_err("diff --cc f\n--- a/f\n+++ b/f\n@@@ -1,1 -1,1 +1,1 @@@\n x\n", 4, "combined");
}

static void test_trailing_garbage_is_ignored(void) {
    Fx x;
    fx_parse(&x,
             "--- a\n"
             "+++ b\n"
             "@@ -1 +1 @@\n"
             " a\n"
             " this extra line is beyond the hunk counts\n");
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    CHECK(hunk_at(file_at(&x, 0), 0)->lines.len == 1);
    fx_free(&x);
}

/* ---- misc API ---- */

static void test_api_helpers(void) {
    CHECK(diffstatus_letter(DIFF_MODIFIED) == 'M');
    CHECK(diffstatus_letter(DIFF_ADDED) == 'A');
    CHECK(diffstatus_letter(DIFF_DELETED) == 'D');
    CHECK(diffstatus_letter(DIFF_RENAMED) == 'R');
    CHECK(diffstatus_letter(DIFF_COPIED) == 'C');
    FileDiff none;
    memset(&none, 0, sizeof none);
    CHECK(strcmp(filediff_path(&none), "(unknown)") == 0);

    Fx x;
    fx_parse(&x, "--- a\n+++ b\n@@ -1 +1 @@\n-a\n+b\n");
    REQUIRE(x.rc == 0);
    patch_free(&x.p);
    CHECK(x.p.files.len == 0 && x.p.files.data == NULL);
    patch_free(&x.p); /* second free is a no-op */
    free(x.buf);
}

/* ---- scale ---- */

static void test_large_inputs(void) {
    Buf b;
    buf_init(&b);
    char tmp[256];

    for (size_t i = 0; i < 2000; i++) {
        int k = snprintf(tmp, sizeof tmp,
                         "diff --git a/f%zu b/f%zu\n--- a/f%zu\n+++ b/f%zu\n@@ -1 +1 @@\n-a\n+b\n",
                         i, i, i, i);
        buf_append(&b, tmp, (size_t)k);
    }
    Fx x;
    fx_parse_n(&x, b.data, b.len);
    REQUIRE(x.rc == 0 && x.p.files.len == 2000);
    CHECK(str_eq(filediff_path(file_at(&x, 1999)), "f1999"));
    fx_free(&x);

    buf_clear(&b);
    buf_puts(&b, "--- a\n+++ b\n@@ -1,50000 +1,50000 @@\n");
    for (size_t i = 0; i < 50000; i++) buf_puts(&b, " line\n");
    fx_parse_n(&x, b.data, b.len);
    REQUIRE(x.rc == 0 && x.p.files.len == 1);
    CHECK(hunk_at(file_at(&x, 0), 0)->lines.len == 50000);
    fx_free(&x);

    /* one 1 MiB line */
    buf_clear(&b);
    buf_puts(&b, "--- a\n+++ b\n@@ -0,0 +1 @@\n+");
    for (size_t i = 0; i < ((size_t)1 << 20); i++) buf_putc(&b, 'x');
    buf_putc(&b, '\n');
    fx_parse_n(&x, b.data, b.len);
    REQUIRE(x.rc == 0);
    CHECK(line_at_(hunk_at(file_at(&x, 0), 0), 0)->len == ((size_t)1 << 20));
    fx_free(&x);

    buf_free(&b);
}

/* ---- deterministic corruption fuzz ---- */

static uint32_t g_rng = 12345u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static const char SAMPLE[] =
    "From 1 Mon Sep 17 00:00:00 2001\nSubject: x\n\n---\n a | 2 +-\n\n"
    "diff --git a/a b/a\nindex 1..2 100644\n--- a/a\n+++ b/a\n"
    "@@ -1,4 +1,4 @@ fn\n one\n-two\n+TWO\n three\n four\n@@ -20 +20,2 @@\n-x\n+y\n+z\n"
    "\\ No newline at end of file\n"
    "diff --git a/n b/n\nnew file mode 100644\n--- /dev/null\n+++ b/n\n@@ -0,0 +1 @@\n+hi\n"
    "diff --git a/o p b/q r\nsimilarity index 90%\nrename from o p\nrename to q r\n"
    "--- a/o p\n+++ b/q r\n@@ -1 +1 @@\n-a\n+b\n"
    "diff --git \"a/w\\tx\" \"b/w\\tx\"\nGIT binary patch\nliteral 1\nz\n\n"
    "diff --git a/m b/m\nold mode 100644\nnew mode 100755\n"
    "--- plain\t2020\n+++ plain2\t2021\n@@ -1 +1 @@\n-1\n+2\n"
    "Binary files x and y differ\n";

static void mutate(Buf *b, const char *src, size_t n) {
    static const char *const ins[] = {"\n",    "@@ -1,2 +1,2 @@\n", "diff --git ",
                                      "\"",    "\\",               "--- ",
                                      "+++ ",  "\r",               "@@@ ",
                                      "\\ No", " ",                "-",
                                      "+"};
    buf_clear(b);
    buf_append(b, src, n);
    uint32_t edits = 1 + rnd() % 10;
    for (uint32_t e = 0; e < edits && b->len > 0; e++) {
        size_t pos = rnd() % b->len;
        switch (rnd() % 5) {
        case 0:
            b->data[pos] = (char)(rnd() & 0xff);
            break;
        case 1: { /* delete a range */
            size_t cnt = 1 + rnd() % 40;
            if (cnt > b->len - pos) cnt = b->len - pos;
            memmove(b->data + pos, b->data + pos + cnt, b->len - pos - cnt);
            b->len -= cnt;
            break;
        }
        case 2: { /* insert a token */
            const char *t = ins[rnd() % (sizeof ins / sizeof ins[0])];
            size_t tl = strlen(t);
            buf_reserve(b, tl);
            memmove(b->data + pos + tl, b->data + pos, b->len - pos);
            memcpy(b->data + pos, t, tl);
            b->len += tl;
            break;
        }
        case 3:
            b->len = pos; /* truncate */
            break;
        default: { /* duplicate a chunk */
            size_t cnt = 1 + rnd() % 60;
            if (cnt > b->len - pos) cnt = b->len - pos;
            char chunk[64];
            memcpy(chunk, b->data + pos, cnt);
            buf_reserve(b, cnt);
            memmove(b->data + pos + cnt, b->data + pos, b->len - pos);
            memcpy(b->data + pos, chunk, cnt);
            b->len += cnt;
        }
        }
        b->data[b->len] = '\0';
    }
}

static int consistent(const Fx *x) {
    if (x->p.files.len == 0) return 0;
    for (size_t i = 0; i < x->p.files.len; i++) {
        const FileDiff *f = vec_at(&x->p.files, i);
        size_t adds = 0, dels = 0;
        for (size_t j = 0; j < f->hunks.len; j++) {
            const Hunk *h = vec_at(&f->hunks, j);
            size_t ctx = 0, a = 0, d = 0;
            for (size_t k = 0; k < h->lines.len; k++) {
                const Line *l = vec_at(&h->lines, k);
                if (l->off > x->n || l->len > x->n - l->off) return 0;
                if (l->kind == LINE_CONTEXT) ctx++;
                else if (l->kind == LINE_ADD) a++;
                else d++;
            }
            if (ctx + d != h->old_count || ctx + a != h->new_count) return 0;
            if (h->head_off > x->n || h->head_len > x->n - h->head_off) return 0;
            adds += a;
            dels += d;
        }
        if (adds != f->adds || dels != f->dels) return 0;
        if (filediff_path(f) == NULL) return 0;
    }
    return 1;
}

static void test_fuzz(void) {
    Buf b;
    buf_init(&b);
    int bad = 0, ok = 0, rejected = 0;
    for (int i = 0; i < 20000; i++) {
        mutate(&b, SAMPLE, sizeof SAMPLE - 1);
        Fx x;
        fx_parse_n(&x, b.data, b.len);
        if (x.rc == 0) {
            ok++;
            if (!consistent(&x)) bad++;
        } else {
            rejected++;
            if (x.err.msg[0] == '\0') bad++;
        }
        fx_free(&x);
    }
    CHECK(bad == 0);
    CHECK(ok > 1000 && rejected > 1000); /* the fuzz must exercise both outcomes */
    buf_free(&b);
}

int main(void) {
    printf("test_patch\n");
    test_basic();
    test_multi_file_multi_hunk();
    test_added_deleted();
    test_rename_copy_mode();
    test_binary();
    test_plain_diff();
    test_no_newline_markers();
    test_crlf();
    test_stripped_empty_context();
    test_hunk_body_looks_like_headers();
    test_mail_preamble();
    test_quoted_and_spaced_paths();
    test_errors();
    test_trailing_garbage_is_ignored();
    test_api_helpers();
    test_large_inputs();
    test_fuzz();
    TEST_DONE();
}
