#include "worddiff.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MIDDLE 512         /* longest token run the quadratic matcher takes for two lines */
#define MAX_BLOCK_MIDDLE 1024  /* ... and for the two sides of a change block. */

typedef struct {
    const char *p; /* the token's bytes; NULL for the line break between two lines of a block */
    size_t len;
    size_t off;  /* byte offset inside its line */
    size_t line; /* index of its line (0 when two single lines are compared) */
    int cls;     /* 0 word, 1 blanks, 2 punctuation, 3 line break */
} Tok;

static int char_class(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
        c >= 0x80)
        return 0; /* word (UTF-8 bytes stay inside their token) */
    if (c == ' ' || c == '\t') return 1;
    return 2; /* punctuation: one token per byte */
}

static void tokenize(const char *s, size_t n, size_t line, Vec *toks) {
    size_t i = 0;
    while (i < n) {
        int c = char_class((unsigned char)s[i]);
        size_t j = i + 1;
        if (c != 2)
            while (j < n && char_class((unsigned char)s[j]) == c) j++;
        Tok *t = vec_push(toks);
        t->p = s + i;
        t->len = j - i;
        t->off = i;
        t->line = line;
        t->cls = c;
        i = j;
    }
}

static void line_break(Vec *toks, size_t line) {
    Tok *t = vec_push(toks); /* zeroed: p is NULL */
    t->line = line;
    t->cls = 3;
}

static bool tok_eq(const Tok *x, const Tok *y) {
    if (x->p == NULL || y->p == NULL) return x->p == y->p; /* a line break equals a line break */
    return x->len == y->len && memcmp(x->p, y->p, x->len) == 0;
}

/* Marks the tokens of the middle parts that belong to a longest common
 * subsequence. ta/tb point at the first middle token, na/nb are the counts. */
static void match_middle(const Tok *ta, size_t na, const Tok *tb, size_t nb, uint8_t *ka,
                         uint8_t *kb) {
    size_t w = nb + 1, cells;
    if (!checked_mul(na + 1, w, &cells)) return;
    uint16_t *dp = xcalloc(cells, sizeof *dp);
    for (size_t i = na; i-- > 0;) {
        for (size_t j = nb; j-- > 0;) {
            uint16_t v;
            if (tok_eq(&ta[i], &tb[j])) {
                v = (uint16_t)(dp[(i + 1) * w + j + 1] + 1);
            } else {
                uint16_t down = dp[(i + 1) * w + j];
                uint16_t right = dp[i * w + j + 1];
                v = down >= right ? down : right;
            }
            dp[i * w + j] = v;
        }
    }
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
        if (tok_eq(&ta[i], &tb[j])) {
            ka[i] = 1;
            kb[j] = 1;
            i++;
            j++;
        } else if (dp[(i + 1) * w + j] >= dp[i * w + j + 1]) {
            i++;
        } else {
            j++;
        }
    }
    free(dp);
}

/* How much a common token is worth when matching a block. Line breaks come
 * first so that lines stay lined up, then words (whatever their length, so
 * that one long word does not outweigh the structure), then punctuation.
 * blanks only ride along. */
static int32_t tok_weight(const Tok *t) {
    switch (t->cls) {
    case 3: return 8;
    case 0: return 4;
    case 2: return 2;
    default: return 0;
    }
}

/* The bonus for matching a[i-1] right after a[i-2] when they also sit side by
 * side in the other text: worth more between two real tokens than next to a
 * blank. */
static int32_t run_bonus(const Tok *ta, size_t i) {
    if (i < 2) return 0;
    return ta[i - 2].cls != 1 && ta[i - 1].cls != 1 ? 2 : 1;
}

 /* Like match_middle, for blocks: a weighted longest common subsequence in
  * which two matches that sit side by side in both texts earn a bonus, so that
  * runs like "ends." or "*" "/" stay together instead of being matched piece by
  * piece to different places. Among equally good matchings the one whose
  * matches come last wins. */
static void match_middle_weighted(const Tok *ta, size_t na, const Tok *tb, size_t nb, uint8_t *ka,
                                  uint8_t *kb) {
    size_t w = nb + 1, cells;
    if (!checked_mul(na + 1, w, &cells)) return;
    /* f: best score of a[0..i) against b[0..j); g: the same when the last
     * tokens a[i-1] and b[j-1] are matched with each other (-1 if they differ) */
    int32_t *f = xcalloc(cells, sizeof *f);
    int32_t *g = xmalloc(cells * sizeof *g);
    for (size_t c = 0; c < cells; c++) g[c] = -1;
    for (size_t i = 1; i <= na; i++) {
        for (size_t j = 1; j <= nb; j++) {
            int32_t up = f[(i - 1) * w + j], left = f[i * w + j - 1];
            int32_t best = up >= left ? up : left;
            if (tok_eq(&ta[i - 1], &tb[j - 1])) {
                int32_t wt = tok_weight(&ta[i - 1]);
                int32_t gv = f[(i - 1) * w + j - 1] + wt;
                int32_t prev = g[(i - 1) * w + j - 1];
                if (prev >= 0 && prev + wt + run_bonus(ta, i) > gv) gv = prev + wt + run_bonus(ta, i);
                g[i * w + j] = gv;
                if (gv > best) best = gv;
            }
            f[i * w + j] = best;
        }
    }
    size_t i = na, j = nb;
    bool chained = false; /* the match above this cell continued a run: this one must be in it */
    while (i > 0 && j > 0) {
        size_t c = i * w + j;
        if (chained || (g[c] >= 0 && f[c] == g[c])) {
            int32_t wt = tok_weight(&ta[i - 1]);
            ka[i - 1] = 1;
            kb[j - 1] = 1;
            int32_t prev = g[(i - 1) * w + j - 1];
            chained = prev >= 0 && prev + wt + run_bonus(ta, i) == g[c];
            i--;
            j--;
        } else if (f[(i - 1) * w + j] >= f[i * w + j - 1]) {
            i--;
        } else {
            j--;
        }
    }
    free(f);
    free(g);
}

/* Gives up small matches that are surrounded by changes, in both texts, and
 * hold no word: a run of blanks, punctuation or line breaks is not worth
 * keeping apart from the changes around it (it would split one change into
 * pieces).
 * Returns true when it changed anything. */
static bool squash(const Tok *ta, size_t na, uint8_t *ka, const Tok *tb, size_t nb, uint8_t *kb) {
    size_t ca = 0, cb = 0;
    for (size_t i = 0; i < na; i++) ca += ka[i];
    for (size_t j = 0; j < nb; j++) cb += kb[j];
    if (ca != cb || ca == 0) return false;
    size_t *ma = xmalloc(ca * sizeof *ma), *mb = xmalloc(cb * sizeof *mb);
    size_t n = 0;
    for (size_t i = 0; i < na; i++)
        if (ka[i]) ma[n++] = i;
    n = 0;
    for (size_t j = 0; j < nb; j++)
        if (kb[j]) mb[n++] = j;

    bool changed = false;
    size_t k = 0;
    while (k < ca) {
        size_t e = k; /* the run of matches [k, e] that sit side by side in both */
        while (e + 1 < ca && ma[e + 1] == ma[e] + 1 && mb[e + 1] == mb[e] + 1) e++;
        bool word = false;
        for (size_t x = k; x <= e; x++)
            if (ta[ma[x]].cls == 0) word = true;
        bool before = ma[k] > 0 && !ka[ma[k] - 1] && mb[k] > 0 && !kb[mb[k] - 1];
        bool after = ma[e] + 1 < na && !ka[ma[e] + 1] && mb[e] + 1 < nb && !kb[mb[e] + 1];
        if (!word && before && after) {
            for (size_t x = k; x <= e; x++) {
                ka[ma[x]] = 0;
                kb[mb[x]] = 0;
            }
            changed = true;
        }
        k = e + 1;
    }
    free(ma);
    free(mb);
    (void)tb;
    return changed;
}

 /* Marks the tokens the two sequences have in common: the equal start, the
  * equal end, and a longest common subsequence of what lies between. Returns
  * false when the middle was too long to match (its tokens stay unmarked). */
static bool match_tokens(const Tok *ta, size_t na, const Tok *tb, size_t nb, uint8_t *ka,
                         uint8_t *kb, size_t max_middle, bool block) {
    size_t pre = 0;
    while (pre < na && pre < nb && tok_eq(&ta[pre], &tb[pre])) {
        ka[pre] = kb[pre] = 1;
        pre++;
    }
    size_t suf = 0;
    while (suf < na - pre && suf < nb - pre && tok_eq(&ta[na - 1 - suf], &tb[nb - 1 - suf])) {
        ka[na - 1 - suf] = kb[nb - 1 - suf] = 1;
        suf++;
    }
    size_t ma = na - pre - suf, mb = nb - pre - suf;
    if (ma == 0 || mb == 0) return true;
    if (ma > max_middle || mb > max_middle) return false;
    if (block) {
        match_middle_weighted(ta + pre, ma, tb + pre, mb, ka + pre, kb + pre);
        while (squash(ta, na, ka, tb, nb, kb)) {
        } 
    } else {
            match_middle(ta + pre, ma, tb + pre, mb, ka + pre, kb + pre);
    }
    return true;
}

static size_t matched_bytes(const Tok *t, size_t n, const uint8_t *kept) {
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (kept[i]) m += t[i].len;
    return m;
}

static bool blank_gap(const char *s, size_t from, size_t to) {
    for (size_t i = from; i < to; i++)
        if (s[i] != ' ' && s[i] != '\t') return false;
    return true;
}

/* Appends the byte ranges of unmatched tokens, joining neighbours and
 * ranges separated only by blanks. */
static void collect(const Tok *t, size_t n, const uint8_t *kept, Vec *out) {
    size_t first = out->len;
    for (size_t i = 0; i < n; i++) {
        if (kept[i]) continue;
        size_t start = t[i].off, end = t[i].off + t[i].len;
        if (out->len > first) {
            Span *last = vec_at(out, out->len - 1);
            if (start == last->end || blank_gap(t[i].p - t[i].off, last->end, start)) {
                last->end = end;
                continue;
            }
        }
        Span *sp = vec_push(out);
        sp->start = start;
        sp->end = end;
    }
}

static bool spacing_only(const Tok *t, size_t n, const uint8_t *kept, size_t i) {
    if (t[i].cls != 1) return false;
    if (i == 0 || t[i - 1].p == NULL) {
        for (size_t j = i + 1; j < n && t[j].p != NULL; j++)
            if (kept[j]) return true;
    }
    return (i + 1 == n || t[i + 1].p == NULL) && i > 0 && kept[i - 1];
}


/* The same for a block: ranges never join across lines, and the line breaks
 * themselves are not marked. */
static void collect_block(const Tok *t, size_t n, const uint8_t *kept, Vec *out) {
    for (size_t i = 0; i < n; i++) {
        if (kept[i] || t[i].p == NULL) continue;
        if (spacing_only(t, n, kept, i)) continue;
        size_t start = t[i].off, end = t[i].off + t[i].len;
        if (out->len > 0) {
            BlockSpan *last = vec_at(out, out->len - 1);
            if (last->line == t[i].line &&
                (start == last->span.end ||
                 blank_gap(t[i].p - t[i].off, last->span.end, start))) {
                last->span.end = end;
                continue;
            }
        }
        BlockSpan *b = vec_push(out);
        b->line = t[i].line;
        b->span.start = start;
        b->span.end = end;
    }
}

bool word_diff(const char *a, size_t an, const char *b, size_t bn, Vec *sa, Vec *sb) {
    if (an > WORDDIFF_MAX_LINE || bn > WORDDIFF_MAX_LINE) return false;

    Vec va, vb;
    vec_init(&va, sizeof(Tok));
    vec_init(&vb, sizeof(Tok));
    tokenize(a, an, 0, &va);
    tokenize(b, bn, 0, &vb);
    size_t na = va.len, nb = vb.len;
    const Tok *ta = va.data, *tb = vb.data;
    uint8_t *ka = xcalloc(na ? na : 1, 1);
    uint8_t *kb = xcalloc(nb ? nb : 1, 1);

    match_tokens(ta, na, tb, nb, ka, kb, MAX_MIDDLE, false);
    
    bool ok = true;
    size_t longer = an > bn ? an : bn;
    if (matched_bytes(ta, na, ka) * 10 < longer * 3) { /* under 30% in common: not worth marking */
        ok = false;
    } else {
        collect(ta, na, ka, sa);
        collect(tb, nb, kb, sb);
    }

    free(ka);
    free(kb);
    vec_free(&va);
    vec_free(&vb);
    return ok;
}

bool word_diff_block(const BlockLine *lines, size_t nd, size_t na, Vec *out) {
    if (nd == 0 || na == 0 || nd + na > WORDDIFF_BLOCK_MAX_LINES) return false;
    size_t bytes_a = 0, bytes_b = 0;
    for (size_t i = 0; i < nd + na; i++) {
        if (lines[i].len > WORDDIFF_MAX_LINE) return false;
        if (i < nd) bytes_a += lines[i].len;
        else bytes_b += lines[i].len;
    }

    Vec va, vb;
    vec_init(&va, sizeof(Tok));
    vec_init(&vb, sizeof(Tok));
    for (size_t i = 0; i < nd; i++) {
        if (i > 0) line_break(&va, i);
        tokenize(lines[i].text, lines[i].len, i, &va);
    }
    for (size_t j = 0; j < na; j++) {
        if (j > 0) line_break(&vb, nd + j);
        tokenize(lines[nd + j].text, lines[nd + j].len, nd + j, &vb);
    }
    size_t n1 = va.len, n2 = vb.len;
    const Tok *ta = va.data, *tb = vb.data;
    uint8_t *ka = xcalloc(n1 ? n1 : 1, 1);
    uint8_t *kb = xcalloc(n2 ? n2 : 1, 1);

    bool ok = match_tokens(ta, n1, tb, n2, ka, kb, MAX_BLOCK_MIDDLE, true);
    size_t shorter = bytes_a < bytes_b ? bytes_a : bytes_b;
    if (ok && matched_bytes(ta, n1, ka) * 10 < shorter * 3) ok = false;
    if (ok) {
        collect_block(ta, n1, ka, out);
        collect_block(tb, n2, kb, out);
    }

    free(ka);
    free(kb);
    vec_free(&va);
    vec_free(&vb);
    return ok;
}
