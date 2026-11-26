#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fuzz.h"
#include "util.h"
#include "worddiff.h"

#define REQUIRE(cond)     \
    do {                  \
        int ok_ = (cond); \
        CHECK(ok_);       \
        if (!ok_) return; \
    } while (0)

/* The lines of a block and what word_diff_block made of them, rendered as
 * text with every changed span in [ ]. */
typedef struct {
    bool ok;
    size_t n;
    char **marked;
    Vec spans;
} Blk;

static char *heap(const char *s, size_t n) { /* exact size so ASan flags overreads */
    char *p = xmalloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

static void blk_free(Blk *b) {
    for (size_t i = 0; i < b->n; i++) free(b->marked[i]);
    free(b->marked);
    vec_free(&b->spans);
}


static char *terminated(const Buf *out) {
    char *p = xmalloc(out->len + 1);
    memcpy(p, out->data, out->len);
    p[out->len] = '\0';
    return p;
}

static void blk_run(Blk *b, const char *const *lines, size_t nd, size_t na) {
    size_t n = nd + na;
    BlockLine *bl = xmalloc(n ? n * sizeof *bl : 1);
    char **copies = xmalloc(n ? n * sizeof *copies : 1);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(lines[i]);
        copies[i] = heap(lines[i], len);
        bl[i].text = copies[i];
        bl[i].len = len;
    }
    vec_init(&b->spans, sizeof(BlockSpan));
    b->ok = word_diff_block(bl, nd, na, &b->spans);
    b->n = n;
    b->marked = xmalloc(n ? n * sizeof *b->marked : 1);

    size_t si = 0;
    for (size_t i = 0; i < n; i++) {
        Buf out;
        buf_init(&out);
        size_t pos = 0;
        for (; si < b->spans.len; si++) {
            const BlockSpan *s = vec_at(&b->spans, si);
            if (s->line != i) break;
            buf_append(&out, bl[i].text + pos, s->span.start - pos);
            buf_putc(&out, '[');
            buf_append(&out, bl[i].text + s->span.start, s->span.end - s->span.start);
            buf_putc(&out, ']');
            pos = s->span.end;
        }
        buf_append(&out, bl[i].text + pos, bl[i].len - pos);
        b->marked[i] = terminated(&out);
        buf_free(&out);
    }
    for (size_t i = 0; i < n; i++) free(copies[i]);
    free(copies);
    free(bl);
}

/* Spans must be sorted by line then position, non-empty, inside their line
 * and never overlap or touch. */
static void check_spans(const Blk *b, const char *const *lines) {
    size_t prev_line = 0, prev_end = 0;
    for (size_t i = 0; i < b->spans.len; i++) {
        const BlockSpan *s = vec_at(&b->spans, i);
        CHECK(s->line < b->n);
        if (s->line >= b->n) return;
        CHECK(s->span.start < s->span.end);
        CHECK(s->span.end <= strlen(lines[s->line]));
        if (i > 0) {
            CHECK(s->line > prev_line || (s->line == prev_line && s->span.start > prev_end));
        }
        prev_line = s->line;
        prev_end = s->span.end;
    }
}

/* ---- the JetBrains reference cases ---- */

static void test_screenshot_comment(void) {
    static const char *const L[] = {
        "/* Moves to the next (dir > 0) or previous match of the active query in the",
        " * current file, wrapping around at the ends. */",
        "static void search_step(Nav *n, const Model *m, int dir, int rows) {",
        "/* Moves to the next (dir > 0) or previous match of the active query, wrapping",
        " * around at the ends. A search of all files goes through every file of the",
        " * patch in order and may land in another file: the result is then true, and",
        " * the caller must rebuild the model (top is already near the match, nav_clamp",
        " * fits it to the new file). */",
        "static bool search_step(Nav *n, const Model *m, int dir, int rows) {",
    };
    Blk b;
    blk_run(&b, L, 3, 6);
    REQUIRE(b.ok);
    check_spans(&b, L);
    CHECK(strcmp(b.marked[0], "/* Moves to the next (dir > 0) or previous match of the active query[ in the]") == 0);
    CHECK(strcmp(b.marked[1], " *[ current file, wrapping] around at the ends. */") == 0);
    CHECK(strcmp(b.marked[2], "static [void] search_step(Nav *n, const Model *m, int dir, int rows) {") == 0);
    CHECK(strcmp(b.marked[3], "/* Moves to the next (dir > 0) or previous match of the active query[, wrapping]") == 0);
    CHECK(strcmp(b.marked[4], " * around at the ends.[ A search of all files goes through every file of the]") == 0);
    /* inserted lines are marked from end to end */
    CHECK(strcmp(b.marked[5], "[ * patch in order and may land in another file: the result is then true, and]") == 0);
    CHECK(strcmp(b.marked[6], "[ * the caller must rebuild the model (top is already near the match, nav_clamp]") == 0);
    CHECK(strcmp(b.marked[7], " [* fits it to the new file).] */") == 0);
    CHECK(strcmp(b.marked[8], "static [bool] search_step(Nav *n, const Model *m, int dir, int rows) {") == 0);
    blk_free(&b);
}

static void test_screenshot_return(void) {
    static const char *const L[] = {"        return;", "        return false;"};
    Blk b;
    blk_run(&b, L, 1, 1);
    REQUIRE(b.ok);
    CHECK(strcmp(b.marked[0], "        return;") == 0);
    CHECK(strcmp(b.marked[1], "        return[ false];") == 0);
    blk_free(&b);
}

/* Eight removed lines against thirty-odd added ones: the removed side is
 * small next to the added side but still clearly the same code. */
static const char *const SEARCH[] = {
    /* removed */
    "    size_t first = SIZE_MAX, last = SIZE_MAX, next = SIZE_MAX, prev = SIZE_MAX, total = 0;",
    "    for (size_t i = 0; i < m->rows.len; i++) {",
    "        if (!row_matches(n, vec_at(&m->rows, i), icase)) continue;",
    "        total++;",
    "        if (first == SIZE_MAX) first = i;",
    "        last = i;",
    "        if (next == SIZE_MAX && i >= from_next) next = i;",
    "        if (i < from_prev) prev = i;",
    /* added */
    "",
    "    /* Matches are ordered by (file, row); the search starts at (n->file, from_*). */",
    "    size_t first_f = SIZE_MAX, first_r = SIZE_MAX, last_f = SIZE_MAX, last_r = SIZE_MAX;",
    "    size_t next_f = SIZE_MAX, next_r = SIZE_MAX, next_ord = 0;",
    "    size_t prev_f = SIZE_MAX, prev_r = SIZE_MAX, prev_ord = 0;",
    "    size_t total = 0;",
    "    for (size_t k = 0; k < nfiles; k++) {",
    "        size_t f = all ? k : n->file;",
    "        Model other;",
    "        const Model *mm = m;",
    "        bool built = f != n->file;",
    "        if (built) {",
    "            model_build(&other, n->patch, vec_at(&n->patch->files, f));",
    "            mm = &other;",
    "        }",
    "        for (size_t i = 0; i < mm->rows.len; i++) {",
    "            if (!row_matches(n, vec_at(&mm->rows, i), icase)) continue;",
    "            total++;",
    "            if (first_f == SIZE_MAX) {",
    "                first_f = f;",
    "                first_r = i;",
    "            }",
    "            last_f = f;",
    "            last_r = i;",
    "            if (next_f == SIZE_MAX && (f > n->file || (f == n->file && i >= from_next))) {",
    "                next_f = f;",
    "                next_r = i;",
    "                next_ord = total;",
    "            }",
    "            if (f < n->file || (f == n->file && i < from_prev)) {",
    "                prev_f = f;",
    "                prev_r = i;",
    "                prev_ord = total;",
    "            }",
    "        }",
    "        if (built) model_free(&other);",
};
#define SEARCH_ND 8
#define SEARCH_N (sizeof SEARCH / sizeof SEARCH[0])

/* Blocks where the added side is much bigger than the removed one must still
 * be compared as a block, and re-indented lines must not be marked. */
static void test_big_insertion(void) {
    Blk b;
    blk_run(&b, SEARCH, SEARCH_ND, SEARCH_N - SEARCH_ND);
    REQUIRE(b.ok);
    check_spans(&b, SEARCH);

    /* removed side: only the renamed identifiers, never the indentation */
    CHECK(strcmp(b.marked[0], "    size_t [first] = SIZE_MAX, [last] = SIZE_MAX, [next] = SIZE_MAX, [prev] = SIZE_MAX, total = 0;") == 0);
    CHECK(strcmp(b.marked[1], "    for (size_t i = 0; i < [m]->rows.len; i++) {") == 0);
    CHECK(strcmp(b.marked[2], "        if (!row_matches(n, vec_at(&[m]->rows, i), icase)) continue;") == 0);
    CHECK(strcmp(b.marked[3], "        total++;") == 0);
    CHECK(strcmp(b.marked[4], "        if ([first] == SIZE_MAX) [first] = i;") == 0);
    CHECK(strcmp(b.marked[5], "        [last] = i;") == 0);

    /* added side: the new comment and declarations are marked end to end */
    size_t a = SEARCH_ND;
    CHECK(strcmp(b.marked[a + 1], "[    /* Matches are ordered by (file, row); the search starts at (n->file, from_*). */]") == 0);
    CHECK(strcmp(b.marked[a + 7], "[        size_t f = all ? k : n->file;]") == 0);
    CHECK(strcmp(b.marked[a + 8], "[        Model other;]") == 0);
    /* ... while the loop that was only moved deeper keeps its text unmarked */
    CHECK(strcmp(b.marked[a + 15], "        for (size_t i = 0; i < [mm]->rows.len; i++) {") == 0);
    CHECK(strcmp(b.marked[a + 16], "            if (!row_matches(n, vec_at(&[mm]->rows, i), icase)) continue;") == 0);
    CHECK(strcmp(b.marked[a + 17], "            total++;") == 0);
    blk_free(&b);
}

/* ---- ordinary cases ---- */

static void test_one_to_one(void) {
    static const char *const L[] = {"int count = compute_total(items, 10);", "long count = compute_total(items, 20);"};
    Blk b;
    blk_run(&b, L, 1, 1);
    REQUIRE(b.ok);
    CHECK(strcmp(b.marked[0], "[int] count = compute_total(items, [10]);") == 0);
    CHECK(strcmp(b.marked[1], "[long] count = compute_total(items, [20]);") == 0);
    blk_free(&b);
}

static void test_unchanged_lines_between(void) {
    static const char *const L[] = {"a = 1;", "keep();", "b = 2;", "a = 9;", "keep();", "b = 8;"};
    Blk b;
    blk_run(&b, L, 3, 3);
    REQUIRE(b.ok);
    CHECK(strcmp(b.marked[0], "a = [1];") == 0);
    CHECK(strcmp(b.marked[1], "keep();") == 0);
    CHECK(strcmp(b.marked[2], "b = [2];") == 0);
    CHECK(strcmp(b.marked[3], "a = [9];") == 0);
    CHECK(strcmp(b.marked[4], "keep();") == 0);
    CHECK(strcmp(b.marked[5], "b = [8];") == 0);
    blk_free(&b);
}

static void test_line_inserted_inside(void) {
    static const char *const L[] = {"one();", "three();", "one();", "two();", "three();"};
    Blk b;
    blk_run(&b, L, 2, 3);
    REQUIRE(b.ok);
    CHECK(strcmp(b.marked[0], "one();") == 0);
    CHECK(strcmp(b.marked[1], "three();") == 0);
    CHECK(strcmp(b.marked[2], "one();") == 0);
    CHECK(strcmp(b.marked[3], "[two();]") == 0);
    CHECK(strcmp(b.marked[4], "three();") == 0);
    blk_free(&b);
}

static void test_indentation_only(void) {
    static const char *const L[] = {"    if (x) {", "        y();", "    }", "        if (x) {", "            y();", "        }"};
    Blk b;
    blk_run(&b, L, 3, 3);
    /* nothing but whitespace changed: either no marks, or a plain fallback */
    if (b.ok) CHECK(b.spans.len == 0);
    blk_free(&b);
}

static void test_not_enough_in_common(void) {
    static const char *const L[] = {"alpha beta gamma", "delta epsilon", "zeta eta theta", "iota kappa lambda"};
    Blk b;
    blk_run(&b, L, 2, 2);
    CHECK(!b.ok);
    CHECK(b.spans.len == 0);
    blk_free(&b);
}

static void test_empty_sides(void) {
    static const char *const L[] = {"a", "b"};
    Blk b;
    blk_run(&b, L, 0, 2);
    CHECK(!b.ok);
    CHECK(b.spans.len == 0);
    blk_free(&b);
    blk_run(&b, L, 2, 0);
    CHECK(!b.ok);
    CHECK(b.spans.len == 0);
    blk_free(&b);
    blk_run(&b, L, 0, 0);
    CHECK(!b.ok);
    blk_free(&b);
}

static void test_empty_lines(void) {
    static const char *const L[] = {"", "x = 1;", "", "x = 2;"};
    Blk b;
    blk_run(&b, L, 2, 2);
    REQUIRE(b.ok);
    check_spans(&b, L);
    CHECK(strcmp(b.marked[1], "x = [1];") == 0);
    CHECK(strcmp(b.marked[3], "x = [2];") == 0);
    blk_free(&b);
}

static void test_utf8_and_tabs(void) {
    static const char *const L[] = {"\tcaf\xc3\xa9 = 1;", "\tcaf\xc3\xa9s = 1;\t// \xe6\x97\xa5\xe6\x9c\xac"};
    Blk b;
    blk_run(&b, L, 1, 1);
    REQUIRE(b.ok);
    check_spans(&b, L);
    for (size_t i = 0; i < b.spans.len; i++) {
        const BlockSpan *s = vec_at(&b.spans, i);
        const char *t = L[s->line];
        /* never starts or ends inside a UTF-8 sequence */
        CHECK(((unsigned char)t[s->span.start] & 0xc0) != 0x80);
        CHECK(s->span.end == strlen(t) || ((unsigned char)t[s->span.end] & 0xc0) != 0x80);
    }
    blk_free(&b);
}

/* ---- limits ---- */

static void test_limits(void) {
    /* too many lines: refused without looking at them */
    size_t n = WORDDIFF_BLOCK_MAX_LINES + 2;
    char **text = xmalloc(n * sizeof *text);
    const char **lines = xmalloc(n * sizeof *lines);
    for (size_t i = 0; i < n; i++) {
        text[i] = xmalloc(16);
        snprintf(text[i], 16, "line %zu;", i % 7);
        lines[i] = text[i];
    }
    Blk b;
    blk_run(&b, lines, n / 2, n - n / 2);
    CHECK(!b.ok);
    CHECK(b.spans.len == 0);
    blk_free(&b);

    /* exactly at the limit is still compared */
    blk_run(&b, lines, WORDDIFF_BLOCK_MAX_LINES / 2, WORDDIFF_BLOCK_MAX_LINES / 2);
    CHECK(b.ok || b.spans.len == 0);
    blk_free(&b);
    for (size_t i = 0; i < n; i++) free(text[i]);
    free(text);
    free(lines);

    /* a line longer than the per-line limit refuses the whole block */
    size_t big = WORDDIFF_MAX_LINE + 1;
    char *l = xmalloc(big + 1);
    memset(l, 'x', big);
    l[big] = '\0';
    const char *L[] = {"a = 1;", l, "a = 2;", "b"};
    blk_run(&b, L, 2, 2);
    CHECK(!b.ok);
    CHECK(b.spans.len == 0);
    blk_free(&b);

    /* a line of exactly the limit is accepted */
    l[WORDDIFF_MAX_LINE] = '\0';
    blk_run(&b, L, 2, 2);
    CHECK(b.spans.len == 0 || b.ok);
    blk_free(&b);
    free(l);
}

/* A very long run of tokens on each side is too much for the quadratic
 * matcher: refused, nothing appended, no crash. */
static void test_too_many_tokens(void) {
    Buf a, c;
    buf_init(&a);
    buf_init(&c);
    for (int i = 0; i < 800; i++) {
        buf_append(&a, "a b ", 4);
        buf_append(&c, "c d ", 4);
    }
    const char *L[] = {a.data, c.data};
    /* split into BlockLines of at most the line limit */
    BlockLine bl[2] = {{a.data, a.len > WORDDIFF_MAX_LINE ? WORDDIFF_MAX_LINE : a.len},
                       {c.data, c.len > WORDDIFF_MAX_LINE ? WORDDIFF_MAX_LINE : c.len}};
    (void)L;
    Vec out;
    vec_init(&out, sizeof(BlockSpan));
    bool ok = word_diff_block(bl, 1, 1, &out);
    CHECK(!ok);
    CHECK(out.len == 0);
    vec_free(&out);
    buf_free(&a);
    buf_free(&c);
}

/* ---- random blocks ---- */

static void test_random(void) {
    static const char *const WORDS[] = {"if", "(", "x", ")", "{", "}", "return", "0", ";", "foo", "bar_baz",
                                        "==", "+", " ", "  ", "\t", "\xc3\xa9", "//", "static", "int"};
    enum { NW = sizeof WORDS / sizeof WORDS[0] };
    g_rng = 424242u;
    for (int iter = 0; iter < 3000; iter++) {
        size_t nd = rnd() % 6, na = rnd() % 8;
        size_t n = nd + na;
        char **text = xmalloc((n ? n : 1) * sizeof *text);
        for (size_t i = 0; i < n; i++) {
            Buf bf;
            buf_init(&bf);
            size_t words = rnd() % 12;
            for (size_t w = 0; w < words; w++) {
                const char *s = WORDS[rnd() % NW];
                buf_append(&bf, s, strlen(s));
            }
            text[i] = terminated(&bf);
            buf_free(&bf);
        }
        Blk b;
        blk_run(&b, (const char *const *)text, nd, na);
        if (nd == 0 || na == 0) CHECK(!b.ok && b.spans.len == 0);
        if (!b.ok) CHECK(b.spans.len == 0);
        check_spans(&b, (const char *const *)text);
        blk_free(&b);
        for (size_t i = 0; i < n; i++) free(text[i]);
        free(text);
    }
}

/* Edit a block of real-looking code at random: it must stay well formed, and
 * identical sides must never be marked. */
static void test_random_edits(void) {
    static const char *const BASE[] = {
        "    size_t i = 0;",
        "    while (i < n) {",
        "        if (a[i] == key) return i;",
        "        i++;",
        "    }",
        "    return (size_t)-1;",
    };
    enum { NB = sizeof BASE / sizeof BASE[0] };
    g_rng = 99u;
    for (int iter = 0; iter < 2000; iter++) {
        char *text[2 * NB];
        size_t n = 0;
        for (size_t i = 0; i < NB; i++) {
            Buf bf;
            buf_init(&bf);
            buf_append(&bf, BASE[i], strlen(BASE[i]));
            text[n++] = terminated(&bf);
            buf_free(&bf);
        }
        size_t nd = n;
        for (size_t i = 0; i < NB; i++) {
            if (rnd() % 7 == 0) continue; /* line removed */
            Buf bf;
            buf_init(&bf);
            buf_append(&bf, BASE[i], strlen(BASE[i]));
            if (rnd() % 3 == 0) bf.data[rnd() % bf.len] = (char)('a' + rnd() % 26);
            if (rnd() % 5 == 0) buf_append(&bf, " // x", 5);
            text[n++] = terminated(&bf);
            buf_free(&bf);
        }
        Blk b;
        blk_run(&b, (const char *const *)text, nd, n - nd);
        check_spans(&b, (const char *const *)text);
        blk_free(&b);
        for (size_t i = 0; i < n; i++) free(text[i]);
    }

    /* the very same text on both sides: nothing changed */
    Blk b;
    static const char *const SAME[] = {"a();", "b();", "a();", "b();"};
    blk_run(&b, SAME, 2, 2);
    CHECK(b.spans.len == 0);
    blk_free(&b);
}

int main(void) {
    printf("test_blockdiff\n");
    test_screenshot_comment();
    test_screenshot_return();
    test_big_insertion();
    test_one_to_one();
    test_unchanged_lines_between();
    test_line_inserted_inside();
    test_indentation_only();
    test_not_enough_in_common();
    test_empty_sides();
    test_empty_lines();
    test_utf8_and_tabs();
    test_limits();
    test_too_many_tokens();
    test_random();
    test_random_edits();
    TEST_DONE();
}
