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
#include "worddiff.h"

#define REQUIRE(cond)     \
    do {                  \
        int ok_ = (cond); \
        CHECK(ok_);       \
        if (!ok_) return; \
    } while (0)

/* ---- word_diff ---- */

/* Exact-size heap copy so ASan flags overreads. */
static char *heap(const char *s, size_t n) {
    char *p = xmalloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

/* Text with every span wrapped in [ ]. */
static void marked(Buf *out, const char *s, size_t n, const Vec *spans) {
    size_t pos = 0;
    for (size_t i = 0; i < spans->len; i++) {
        const Span *sp = vec_at(spans, i);
        buf_append(out, s + pos, sp->start - pos);
        buf_putc(out, '[');
        buf_append(out, s + sp->start, sp->end - sp->start);
        buf_putc(out, ']');
        pos = sp->end;
    }
    buf_append(out, s + pos, n - pos);
}

typedef struct {
    bool ok;
    Buf a, b; /* marked texts */
    Vec sa, sb;
} Wd;

static void wd_run(Wd *w, const char *a, size_t an, const char *b, size_t bn) {
    char *ca = heap(a, an), *cb = heap(b, bn);
    vec_init(&w->sa, sizeof(Span));
    vec_init(&w->sb, sizeof(Span));
    w->ok = word_diff(ca, an, cb, bn, &w->sa, &w->sb);
    buf_init(&w->a);
    buf_init(&w->b);
    marked(&w->a, ca, an, &w->sa);
    marked(&w->b, cb, bn, &w->sb);
    free(ca);
    free(cb);
}

static void wd_free(Wd *w) {
    buf_free(&w->a);
    buf_free(&w->b);
    vec_free(&w->sa);
    vec_free(&w->sb);
}

#define WD(wvar, a, b) wd_run(&(wvar), a, strlen(a), b, strlen(b))

static void spans_valid(const Vec *sp, size_t n) {
    size_t prev_end = 0;
    for (size_t i = 0; i < sp->len; i++) {
        const Span *s = vec_at(sp, i);
        CHECK(s->start < s->end);
        CHECK(s->end <= n);
        CHECK(i == 0 || s->start > prev_end); /* sorted, never touching */
        prev_end = s->end;
    }
}

static void test_basic(void) {
    Wd w;

    WD(w, "same line", "same line");
    CHECK(w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);

    WD(w, "", "");
    CHECK(w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);

    WD(w, "int count = compute_total(items, 10);", "long count = compute_total(items, 20);");
    CHECK(w.ok);
    CHECK(strcmp(w.a.data, "[int] count = compute_total(items, [10]);") == 0);
    CHECK(strcmp(w.b.data, "[long] count = compute_total(items, [20]);") == 0);
    wd_free(&w);

    /* pure insertion / deletion */
    WD(w, "return count;", "return count + 1;");
    CHECK(w.ok);
    CHECK(strcmp(w.a.data, "return count;") == 0);
    CHECK(strcmp(w.b.data, "return count[ + 1];") == 0);
    wd_free(&w);
    WD(w, "return count + 1;", "return count;");
    CHECK(strcmp(w.a.data, "return count[ + 1];") == 0);
    CHECK(strcmp(w.b.data, "return count;") == 0);
    wd_free(&w);

    /* a changed word at the very start and the very end */
    WD(w, "alpha beta gamma delta", "ALPHA beta gamma DELTA");
    CHECK(strcmp(w.a.data, "[alpha] beta gamma [delta]") == 0);
    CHECK(strcmp(w.b.data, "[ALPHA] beta gamma [DELTA]") == 0);
    wd_free(&w);

    /* blanks between two changed words are joined into one mark */
    WD(w, "call(foo bar, common_stuff_here)", "call(xx yy, common_stuff_here)");
    CHECK(strcmp(w.a.data, "call([foo bar], common_stuff_here)") == 0);
    CHECK(strcmp(w.b.data, "call([xx yy], common_stuff_here)") == 0);
    wd_free(&w);

    /* but a changed token between unchanged words is marked alone */
    WD(w, "x = a + b * c;", "x = a - b * c;");
    CHECK(strcmp(w.a.data, "x = a [+] b * c;") == 0);
    CHECK(strcmp(w.b.data, "x = a [-] b * c;") == 0);
    wd_free(&w);

    /* tabs count as blanks */
    WD(w, "\tfoo bar", "\tfoo baz");
    CHECK(strcmp(w.a.data, "\tfoo [bar]") == 0);
    CHECK(strcmp(w.b.data, "\tfoo [baz]") == 0);
    wd_free(&w);

    /* indentation changes are marked */
    WD(w, "    value = 1;", "        value = 1;");
    CHECK(w.ok);
    CHECK(w.sa.len == 1 && w.sb.len == 1);
    wd_free(&w);

    /* repeated words: the marks still describe a valid edit */
    WD(w, "a a a a b", "a a a a c");
    CHECK(strcmp(w.a.data, "a a a a [b]") == 0);
    CHECK(strcmp(w.b.data, "a a a a [c]") == 0);
    wd_free(&w);
}

static void test_dissimilar(void) {
    Wd w;
    WD(w, "completely different", "nothing alike here at all");
    CHECK(!w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);

    WD(w, "", "abc");
    CHECK(!w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);

    WD(w, "abc", "");
    CHECK(!w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);

    WD(w, "a", "b");
    CHECK(!w.ok);
    wd_free(&w);

    /* exactly at the boundary: 3 of 10 bytes shared is enough */
    WD(w, "abc.......", "abc,,,,,,,");
    CHECK(w.ok);
    wd_free(&w);
    WD(w, "ab........", "ab,,,,,,,,");
    CHECK(!w.ok);
    wd_free(&w);
}

static void test_limits(void) {
    /* lines over the limit are refused without being tokenised */
    size_t n = WORDDIFF_MAX_LINE + 1;
    char *a = xmalloc(n), *b = xmalloc(n);
    memset(a, 'x', n);
    memset(b, 'x', n);
    b[100] = 'y';
    Wd w;
    wd_run(&w, a, n, b, n);
    CHECK(!w.ok && w.sa.len == 0 && w.sb.len == 0);
    wd_free(&w);
    wd_run(&w, a, n - 1, b, n - 1); /* exactly at the limit: one word each, differing */
    CHECK(w.sa.len <= 1 && w.sb.len <= 1);
    spans_valid(&w.sa, n - 1);
    spans_valid(&w.sb, n - 1);
    wd_free(&w);

    /* only one side too long */
    wd_run(&w, "short", 5, a, n);
    CHECK(!w.ok);
    wd_free(&w);
    free(a);
    free(b);

    /* more than 512 differing tokens between a long common prefix: the
     * middle is marked as one block instead of being matched */
    Buf pa, pb;
    buf_init(&pa);
    buf_init(&pb);
    for (int i = 0; i < 300; i++) {
        buf_puts(&pa, "word ");
        buf_puts(&pb, "word ");
    }
    size_t prefix = pa.len;
    for (int i = 0; i < 600; i++) {
        buf_puts(&pa, "x,");
        buf_puts(&pb, "y,");
    }
    wd_run(&w, pa.data, pa.len, pb.data, pb.len);
    CHECK(w.ok);
    CHECK(w.sa.len == 1 && w.sb.len == 1);
    if (w.sa.len == 1) {
        const Span *s = vec_at(&w.sa, 0);
        CHECK(s->start == prefix && s->end == pa.len - 1); /* the final "," is shared */
    }
    wd_free(&w);
    buf_free(&pa);
    buf_free(&pb);

    /* a few hundred tokens still go through the exact matcher */
    buf_init(&pa);
    buf_init(&pb);
    for (int i = 0; i < 200; i++) {
        char t[16];
        snprintf(t, sizeof t, "w%d ", i);
        buf_puts(&pa, t);
        if (i != 100) buf_puts(&pb, t);
    }
    wd_run(&w, pa.data, pa.len, pb.data, pb.len);
    CHECK(w.ok && w.sa.len == 1 && w.sb.len == 0);
    CHECK(strstr(w.a.data, "[w100 ]") != NULL || strstr(w.a.data, "[w100]") != NULL);
    wd_free(&w);
    buf_free(&pa);
    buf_free(&pb);
}

static void test_utf8(void) {
    Wd w;
    WD(w, "na\xc3\xafve caf\xc3\xa9 shop", "na\xc3\xafve caf\xc3\xa9s shop");
    CHECK(w.ok);
    CHECK(strcmp(w.a.data, "na\xc3\xafve [caf\xc3\xa9] shop") == 0);
    CHECK(strcmp(w.b.data, "na\xc3\xafve [caf\xc3\xa9s] shop") == 0);
    wd_free(&w);

    WD(w, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e abc", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e abd");
    CHECK(strcmp(w.a.data, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e [abc]") == 0);
    wd_free(&w);

    /* marks never start or end inside a multi-byte character */
    for (int iter = 0; iter < 3000; iter++) {
        char a[48], b[48];
        size_t an = rnd() % sizeof a, bn = rnd() % sizeof b;
        static const char *const pieces[] = {"a", "b", " ", ",", "\xc3\xa9", "\xe6\x97\xa5",
                                             "\xf0\x9f\x98\x80", "xy"};
        size_t k = 0;
        while (k + 4 < an) {
            const char *p = pieces[rnd() % 8];
            size_t l = strlen(p);
            memcpy(a + k, p, l);
            k += l;
        }
        an = k;
        k = 0;
        while (k + 4 < bn) {
            const char *p = pieces[rnd() % 8];
            size_t l = strlen(p);
            memcpy(b + k, p, l);
            k += l;
        }
        bn = k;
        wd_run(&w, a, an, b, bn);
        for (size_t i = 0; i < w.sa.len; i++) {
            const Span *s = vec_at(&w.sa, i);
            CHECK(((unsigned char)a[s->start] & 0xC0) != 0x80);
            CHECK(s->end == an || ((unsigned char)a[s->end] & 0xC0) != 0x80);
        }
        for (size_t i = 0; i < w.sb.len; i++) {
            const Span *s = vec_at(&w.sb, i);
            CHECK(((unsigned char)b[s->start] & 0xC0) != 0x80);
            CHECK(s->end == bn || ((unsigned char)b[s->end] & 0xC0) != 0x80);
        }
        wd_free(&w);
    }
}

/* One unique word replaced in a line of unique words: the marks are exactly
 * that word on both sides. */
static void test_single_word_edit(void) {
    for (int iter = 0; iter < 2000; iter++) {
        Buf a, b;
        buf_init(&a);
        buf_init(&b);
        size_t words = 6 + rnd() % 10;
        size_t edit = rnd() % words;
        size_t sa = 0, ea = 0, sb = 0, eb = 0;
        for (size_t i = 0; i < words; i++) {
            char w[48];
            size_t letters = 3 + rnd() % 6;
            char word[16];
            for (size_t j = 0; j < letters; j++) word[j] = (char)('a' + rnd() % 26);
            word[letters] = '\0';
            snprintf(w, sizeof w, "%s%zu", word, i);
            const char *sep = rnd() % 3 == 0 ? ", " : rnd() % 2 ? " " : "  ";
            if (i) {
                buf_puts(&a, sep);
                buf_puts(&b, sep);
            }
            if (i == edit) {
                sa = a.len;
                sb = b.len;
                buf_puts(&a, w);
                buf_puts(&b, "QQQQQ");
                ea = a.len;
                eb = b.len;
            } else {
                buf_puts(&a, w);
                buf_puts(&b, w);
            }
        }
        Wd x;
        wd_run(&x, a.data, a.len, b.data, b.len);
        CHECK(x.ok);
        CHECK(x.sa.len == 1 && x.sb.len == 1);
        if (x.sa.len == 1 && x.sb.len == 1) {
            const Span *p = vec_at(&x.sa, 0), *q = vec_at(&x.sb, 0);
            CHECK(p->start == sa && p->end == ea);
            CHECK(q->start == sb && q->end == eb);
        }
        wd_free(&x);
        buf_free(&a);
        buf_free(&b);
    }
}

/* Arbitrary bytes: never reads outside the lines, spans are always valid. */
static void test_random(void) {
    static const char alpha[] = "ab \t,.;(){}_9\xc3\xa9\xff\x01\x1b";
    for (int iter = 0; iter < 20000; iter++) {
        size_t an = rnd() % 40, bn = rnd() % 40;
        char a[40], b[40];
        for (size_t i = 0; i < an; i++) a[i] = alpha[rnd() % (sizeof alpha - 1)];
        for (size_t i = 0; i < bn; i++) b[i] = alpha[rnd() % (sizeof alpha - 1)];
        Wd w;
        wd_run(&w, a, an, b, bn);
        spans_valid(&w.sa, an);
        spans_valid(&w.sb, bn);
        if (!w.ok) CHECK(w.sa.len == 0 && w.sb.len == 0);
        wd_free(&w);

        /* a line against itself never has marks */
        wd_run(&w, a, an, a, an);
        CHECK(w.ok && w.sa.len == 0 && w.sb.len == 0);
        wd_free(&w);
    }
}

/* ---- row pairing ---- */

typedef struct {
    Patch p;
    Model m;
    char *buf;
    int built;
} Mx;

static int mx_open(Mx *x, const char *s, size_t n) {
    ParseError err;
    x->buf = heap(s, n);
    x->built = 0;
    int rc = patch_parse(&x->p, x->buf, n, &err);
    if (rc == 0 && x->p.files.len > 0) {
        model_build(&x->m, &x->p, vec_at(&x->p.files, 0));
        x->built = 1;
    }
    return x->built;
}

static void mx_close(Mx *x) {
    if (x->built) model_free(&x->m);
    patch_free(&x->p);
    free(x->buf);
}

static const Row *row(const Mx *x, size_t i) { return vec_at(&x->m.rows, i); }

static void expect_pairs(const Mx *x, const char *what, const long *pairs) {
    /* pairs[i] = partner index of row i, or -1 */
    (void)what;
    for (size_t i = 0; i < x->m.rows.len; i++) {
        const Row *r = row(x, i);
        if (pairs[i] < 0) {
            CHECK(!r->has_pair);
        } else {
            CHECK(r->has_pair && r->pair == (size_t)pairs[i]);
        }
    }
}

#define HDR "--- a/f\n+++ b/f\n"

static void test_pairing(void) {
    Mx x;

    /* -a -b +c +d  ->  a<->c, b<->d */
    {
        static const char P[] = HDR "@@ -1,2 +1,2 @@\n-a\n-b\n+c\n+d\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, 3, 4, 1, 2};
        CHECK(x.m.rows.len == 5);
        if (x.m.rows.len == 5) expect_pairs(&x, "2x2", want);
        mx_close(&x);
    }
    /* more removed than added: only the first rows pair */
    {
        static const char P[] = HDR "@@ -1,3 +1 @@\n-a\n-b\n-c\n+d\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, 4, -1, -1, 1};
        CHECK(x.m.rows.len == 5);
        if (x.m.rows.len == 5) expect_pairs(&x, "3x1", want);
        mx_close(&x);
    }
    /* more added than removed */
    {
        static const char P[] = HDR "@@ -1 +1,3 @@\n-a\n+b\n+c\n+d\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, 2, 1, -1, -1};
        CHECK(x.m.rows.len == 5);
        if (x.m.rows.len == 5) expect_pairs(&x, "1x3", want);
        mx_close(&x);
    }
    /* only adds, only removes, only context */
    {
        static const char P[] = HDR "@@ -0,0 +1,2 @@\n+a\n+b\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, -1, -1};
        expect_pairs(&x, "adds", want);
        mx_close(&x);
    }
    {
        static const char P[] = HDR "@@ -1,2 +0,0 @@\n-a\n-b\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, -1, -1};
        expect_pairs(&x, "dels", want);
        mx_close(&x);
    }
    /* interleaved blocks separated by context pair independently */
    {
        static const char P[] = HDR "@@ -1,4 +1,4 @@\n-a\n+b\n k\n-c\n+d\n m\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, 2, 1, -1, 5, 4, -1};
        CHECK(x.m.rows.len == 7);
        if (x.m.rows.len == 7) expect_pairs(&x, "two blocks", want);
        mx_close(&x);
    }
    /* adds before removes in the patch do not pair (git always writes
     * removals first) */
    {
        static const char P[] = HDR "@@ -1 +1 @@\n+b\n-a\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, -1, -1};
        expect_pairs(&x, "add first", want);
        mx_close(&x);
    }
    /* a block never pairs across a hunk boundary */
    {
        static const char P[] = HDR "@@ -1 +1,0 @@\n-a\n@@ -9,0 +8 @@\n+b\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, -1, -1, -1};
        CHECK(x.m.rows.len == 4);
        if (x.m.rows.len == 4) expect_pairs(&x, "across hunks", want);
        mx_close(&x);
    }
    /* the no-newline marker does not break a block */
    {
        static const char P[] = HDR "@@ -1 +1 @@\n-a\n\\ No newline at end of file\n+b\n\\ No newline at end of file\n";
        REQUIRE(mx_open(&x, P, sizeof P - 1));
        static const long want[] = {-1, 2, 1};
        CHECK(x.m.rows.len == 3);
        if (x.m.rows.len == 3) expect_pairs(&x, "no eol", want);
        mx_close(&x);
    }
}

static void test_pairing_invariants(void) {
    static const char BASE[] =
        HDR "@@ -1,5 +1,5 @@\n-a\n-b\n+c\n+d\n k\n-e\n+f\n m\n@@ -20,2 +20,3 @@\n-x\n+y\n+z\n q\n";
    Buf m;
    buf_init(&m);
    int built = 0;
    for (int iter = 0; iter < 20000; iter++) {
        mutate(&m, BASE, sizeof BASE - 1);
        Mx x;
        if (!mx_open(&x, m.data, m.len)) {
            patch_free(&x.p);
            free(x.buf);
            continue;
        }
        built++;
        for (size_t i = 0; i < x.m.rows.len; i++) {
            const Row *r = row(&x, i);
            if (!r->has_pair) continue;
            CHECK(r->kind == ROW_DEL || r->kind == ROW_ADD);
            CHECK(r->pair < x.m.rows.len);
            if (r->pair >= x.m.rows.len) continue;
            const Row *q = row(&x, r->pair);
            CHECK(q->has_pair && q->pair == i);
            CHECK(r->kind == ROW_DEL ? q->kind == ROW_ADD : q->kind == ROW_DEL);
            if (r->kind == ROW_DEL) CHECK(r->pair > i);
            else CHECK(r->pair < i);
        }
        mx_close(&x);
    }
    CHECK(built > 500); /* the mutator leaves enough parseable inputs */
    buf_free(&m);
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

static void draw(const Mx *x, size_t left, int rows, int cols, bool tc, Scr *s) {
    RenderView v;
    memset(&v, 0, sizeof v);
    v.model = &x->m;
    v.file_count = 1;
    v.left = left;
    v.rows = rows;
    v.cols = cols;
    v.truecolor = tc;
    Buf f;
    buf_init(&f);
    render_frame(&f, &v);
    scr_parse(s, &f);
    buf_free(&f);
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

#define ADD_WORD_T "48;2;34;112;64"
#define DEL_WORD_T "48;2;140;48;56"

/* The text drawn right after each occurrence of the style (up to the next
 * escape), concatenated. */
static void word_text(const Buf *raw, const char *style_needle, Buf *out) {
    const char *p = raw->data;
    for (;;) {
        const char *hit = strstr(p, style_needle);
        if (!hit) return;
        const char *m = strchr(hit, 'm');
        if (!m) return;
        m++;
        const char *e = strchr(m, '\x1b');
        size_t n = e ? (size_t)(e - m) : strlen(m);
        buf_append(out, m, n);
        p = m;
    }
}

static void test_render_marks(void) {
    static const char P[] =
        HDR "@@ -1,2 +1,2 @@\n-int count = compute_total(items, 10);\n-return count;\n"
            "+long count = compute_total(items, 20);\n+return count + 1;\n";
    Mx x;
    Scr s;
    REQUIRE(mx_open(&x, P, sizeof P - 1));
    /* rows: 0 hunk, 1 del, 2 del, 3 add, 4 add -> screen rows 3..6 */
    draw(&x, 0, 12, 80, true, &s);
    CHECK(geometry_ok(&s, 12, 80));

    Buf t;
    buf_init(&t);
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "int10") == 0);
    buf_clear(&t);
    word_text(&s.raw[2], ADD_WORD_T, &t);
    CHECK(t.len == 0); /* a removed row never uses the added-word color */
    buf_clear(&t);
    word_text(&s.raw[4], ADD_WORD_T, &t);
    CHECK(strcmp(t.data, "long20") == 0);
    buf_clear(&t);
    word_text(&s.raw[4], DEL_WORD_T, &t);
    CHECK(t.len == 0);

    /* "return count;" has nothing removed: no marks on the removed row, the
     * inserted " + 1" is marked on the added row */
    CHECK(strstr(s.raw[3].data, DEL_WORD_T) == NULL);
    buf_clear(&t);
    word_text(&s.raw[5], ADD_WORD_T, &t);
    CHECK(strcmp(t.data, " + 1") == 0);

    /* plain text is exactly the line, marked or not */
    CHECK(strstr(s.plain[2].data, "int count = compute_total(items, 10);") != NULL);
    CHECK(strstr(s.plain[4].data, "long count = compute_total(items, 20);") != NULL);
    buf_free(&t);
    scr_free(&s);

    draw(&x, 0, 12, 80, false, &s);
    CHECK(strstr(s.raw[2].data, "48;5;88") != NULL);
    CHECK(strstr(s.raw[4].data, "48;5;28") != NULL);
    CHECK(strstr(s.raw[2].data, "48;2;") == NULL);
    scr_free(&s);
    mx_close(&x);
}

static void test_render_no_marks(void) {
    static const char P[] =
        HDR "@@ -1,2 +1,3 @@\n-completely different\n+nothing alike here at all\n+solo\n k\n";
    Mx x;
    Scr s;
    REQUIRE(mx_open(&x, P, sizeof P - 1));
    draw(&x, 0, 10, 60, true, &s);
    CHECK(geometry_ok(&s, 10, 60));
    for (int r = 2; r <= 6; r++) {
        CHECK(strstr(s.raw[r - 1].data, ADD_WORD_T) == NULL);
        CHECK(strstr(s.raw[r - 1].data, DEL_WORD_T) == NULL);
    }
    /* whole-row colors are still there */
    CHECK(strstr(s.raw[2].data, "48;2;82;38;42") != NULL);
    CHECK(strstr(s.raw[3].data, "48;2;36;68;46") != NULL);
    scr_free(&s);
    mx_close(&x);
}

static void test_render_padding(void) {
    /* the mark ends the line: padding after it must not be highlighted */
    static const char P[] = HDR "@@ -1 +1 @@\n-foo bar baz\n+foo bar qux\n";
    Mx x;
    Scr s;
    REQUIRE(mx_open(&x, P, sizeof P - 1));
    draw(&x, 0, 8, 40, true, &s);
    CHECK(geometry_ok(&s, 8, 40));
    for (int r = 3; r <= 4; r++) {
        const char *raw = s.raw[r - 1].data;
        const char *last = NULL;
        for (const char *p = raw; (p = strstr(p, r == 3 ? DEL_WORD_T : ADD_WORD_T)) != NULL; p++) last = p;
        CHECK(last != NULL);
        if (!last) continue;
        int escapes = 0;
        for (const char *p = last + 1; *p; p++)
            if (*p == '\x1b') escapes++;
        CHECK(escapes >= 2); /* back to the row color, then the final reset */
        CHECK(raw[strlen(raw) - 1] == 'm');
    }
    Buf t;
    buf_init(&t);
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "baz") == 0);
    buf_free(&t);
    scr_free(&s);
    mx_close(&x);
}

static void test_render_scrolled(void) {
    static const char P[] =
        HDR "@@ -1 +1 @@\n-xxxxxxxxxx changed tail text here\n+xxxxxxxxxx altered tail text here\n";
    Mx x;
    Scr s;
    REQUIRE(mx_open(&x, P, sizeof P - 1));
    Buf t;
    buf_init(&t);

    /* gutter is 5 columns; "changed" sits at text columns 11..17 */
    draw(&x, 0, 8, 60, true, &s);
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "changed") == 0);
    scr_free(&s);

    buf_clear(&t);
    draw(&x, 14, 8, 60, true, &s); /* first four letters scrolled off */
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "nged") == 0);
    CHECK(geometry_ok(&s, 8, 60));
    scr_free(&s);

    buf_clear(&t);
    draw(&x, 18, 8, 60, true, &s); /* the whole mark is off screen */
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(t.len == 0);
    CHECK(strstr(s.plain[2].data, "tail text here") != NULL);
    scr_free(&s);

    buf_clear(&t);
    draw(&x, 0, 8, 5 + 14, true, &s); /* the window ends in the middle of the mark */
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "cha") == 0);
    CHECK(geometry_ok(&s, 8, 19));
    scr_free(&s);
    buf_free(&t);

    /* every window position keeps every row exactly as wide as the screen */
    for (int cols = 1; cols <= 50; cols++) {
        for (size_t left = 0; left < 40; left += 3) {
            draw(&x, left, 8, cols, cols % 2, &s);
            CHECK(geometry_ok(&s, 8, cols));
            scr_free(&s);
        }
    }
    mx_close(&x);
}

static void test_render_wide(void) {
    static const char P[] = HDR "@@ -1 +1 @@\n-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e abc tail\n"
                                "+\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e abd tail\n";
    Mx x;
    Scr s;
    REQUIRE(mx_open(&x, P, sizeof P - 1));
    for (int cols = 1; cols <= 30; cols++) {
        for (size_t left = 0; left < 14; left++) {
            draw(&x, left, 8, cols, true, &s);
            CHECK(geometry_ok(&s, 8, cols));
            scr_free(&s);
        }
    }
    draw(&x, 0, 8, 40, true, &s);
    Buf t;
    buf_init(&t);
    word_text(&s.raw[2], DEL_WORD_T, &t);
    CHECK(strcmp(t.data, "abc") == 0);
    buf_free(&t);
    scr_free(&s);
    mx_close(&x);
}

static void test_render_fuzz(void) {
    static const char BASE[] =
        HDR "@@ -1,4 +1,4 @@\n-let value = old_name(a, b);\n-return value;\n"
            "+let value = new_name(a, b, c);\n+return value + 1;\n k\n-tail line\n+tail LINE\n";
    Buf m;
    buf_init(&m);
    int rendered = 0;
    for (int iter = 0; iter < 3000; iter++) {
        mutate(&m, BASE, sizeof BASE - 1);
        Mx x;
        if (!mx_open(&x, m.data, m.len)) {
            patch_free(&x.p);
            free(x.buf);
            continue;
        }
        int rows = (int)(1 + rnd() % 30), cols = (int)(1 + rnd() % 90);
        Scr s;
        draw(&x, rnd() % 30, rows, cols, rnd() & 1, &s);
        CHECK(geometry_ok(&s, rows, cols));
        rendered++;
        scr_free(&s);
        mx_close(&x);
    }
    CHECK(rendered > 300);
    buf_free(&m);
}

int main(void) {
    printf("test_worddiff\n");
    test_basic();
    test_dissimilar();
    test_limits();
    test_utf8();
    test_single_word_edit();
    test_random();
    test_pairing();
    test_pairing_invariants();
    test_render_marks();
    test_render_no_marks();
    test_render_padding();
    test_render_scrolled();
    test_render_wide();
    test_render_fuzz();
    TEST_DONE();
}
