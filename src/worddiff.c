#include "worddiff.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MIDDLE 512 /* longest token run the quadratic matcher will take */

typedef struct {
    size_t off, len;
} Tok;

static int char_class(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
        c >= 0x80)
        return 0; /* word (UTF-8 bytes stay inside their token) */
    if (c == ' ' || c == '\t') return 1;
    return 2; /* punctuation: one token per byte */
}

static void tokenize(const char *s, size_t n, Vec *toks) {
    size_t i = 0;
    while (i < n) {
        int c = char_class((unsigned char)s[i]);
        size_t j = i + 1;
        if (c != 2)
            while (j < n && char_class((unsigned char)s[j]) == c) j++;
        Tok *t = vec_push(toks);
        t->off = i;
        t->len = j - i;
        i = j;
    }
}

static bool tok_eq(const char *a, const Tok *x, const char *b, const Tok *y) {
    return x->len == y->len && memcmp(a + x->off, b + y->off, x->len) == 0;
}

/* Marks the tokens of the middle parts that belong to a longest common
 * subsequence. ta/tb point at the first middle token, na/nb are the counts. */
static void match_middle(const char *a, const Tok *ta, size_t na, const char *b, const Tok *tb,
                         size_t nb, uint8_t *ka, uint8_t *kb) {
    size_t w = nb + 1, cells;
    if (!checked_mul(na + 1, w, &cells)) return;
    uint16_t *dp = xcalloc(cells, sizeof *dp);
    for (size_t i = na; i-- > 0;) {
        for (size_t j = nb; j-- > 0;) {
            uint16_t v;
            if (tok_eq(a, &ta[i], b, &tb[j])) {
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
        if (tok_eq(a, &ta[i], b, &tb[j])) {
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

static bool blank_gap(const char *s, size_t from, size_t to) {
    for (size_t i = from; i < to; i++)
        if (s[i] != ' ' && s[i] != '\t') return false;
    return true;
}

/* Appends the byte ranges of unmatched tokens, joining neighbours and
 * ranges separated only by blanks. */
static void collect(const char *s, const Tok *t, size_t n, const uint8_t *kept, Vec *out) {
    size_t first = out->len;
    for (size_t i = 0; i < n; i++) {
        if (kept[i]) continue;
        size_t start = t[i].off, end = t[i].off + t[i].len;
        if (out->len > first) {
            Span *last = vec_at(out, out->len - 1);
            if (start == last->end || blank_gap(s, last->end, start)) {
                last->end = end;
                continue;
            }
        }
        Span *sp = vec_push(out);
        sp->start = start;
        sp->end = end;
    }
}

bool word_diff(const char *a, size_t an, const char *b, size_t bn, Vec *sa, Vec *sb) {
    if (an > WORDDIFF_MAX_LINE || bn > WORDDIFF_MAX_LINE) return false;

    Vec va, vb;
    vec_init(&va, sizeof(Tok));
    vec_init(&vb, sizeof(Tok));
    tokenize(a, an, &va);
    tokenize(b, bn, &vb);
    size_t na = va.len, nb = vb.len;
    const Tok *ta = va.data, *tb = vb.data;
    uint8_t *ka = xcalloc(na ? na : 1, 1);
    uint8_t *kb = xcalloc(nb ? nb : 1, 1);

    size_t pre = 0;
    while (pre < na && pre < nb && tok_eq(a, &ta[pre], b, &tb[pre])) {
        ka[pre] = kb[pre] = 1;
        pre++;
    }
    size_t suf = 0;
    while (suf < na - pre && suf < nb - pre && tok_eq(a, &ta[na - 1 - suf], b, &tb[nb - 1 - suf])) {
        ka[na - 1 - suf] = kb[nb - 1 - suf] = 1;
        suf++;
    }
    size_t ma = na - pre - suf, mb = nb - pre - suf;
    if (ma > 0 && mb > 0 && ma <= MAX_MIDDLE && mb <= MAX_MIDDLE)
        match_middle(a, ta + pre, ma, b, tb + pre, mb, ka + pre, kb + pre);

    size_t matched = 0;
    for (size_t i = 0; i < na; i++)
        if (ka[i]) matched += ta[i].len;

    bool ok = true;
    size_t longer = an > bn ? an : bn;
    if (matched * 10 < longer * 3) { /* under 30% in common: not worth marking */
        ok = false;
    } else {
        collect(a, ta, na, ka, sa);
        collect(b, tb, nb, kb, sb);
    }

    free(ka);
    free(kb);
    vec_free(&va);
    vec_free(&vb);
    return ok;
}