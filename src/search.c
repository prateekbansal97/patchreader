#include "search.h"

#include <stdint.h>

#include "worddiff.h"

static unsigned char fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

bool search_icase(const char *q, size_t qn) {
    for (size_t i = 0; i < qn; i++)
        if (q[i] >= 'A' && q[i] <= 'Z') return false;
    return true;
}

size_t search_find(const char *s, size_t n, const char *q, size_t qn, bool icase, size_t from) {
    if (qn == 0 || qn > n || from > n - qn) return SIZE_MAX;
    unsigned char first = (unsigned char)q[0];
    if (icase) first = fold(first);
    for (size_t i = from; i <= n - qn; i++) {
        unsigned char c = (unsigned char)s[i];
        if (icase) c = fold(c);
        if (c != first) continue;
        size_t k = 1;
        for (; k < qn; k++) {
            unsigned char a = (unsigned char)s[i + k], b = (unsigned char)q[k];
            if (icase) {
                a = fold(a);
                b = fold(b);
            }
            if (a != b) break;
        }
        if (k == qn) return i;
    }
    return SIZE_MAX;
}

void search_spans(const char *s, size_t n, const char *q, size_t qn, bool icase, Vec *spans) {
    size_t pos = 0;
    while (spans->len < SEARCH_MAX_SPANS) {
        size_t at = search_find(s, n, q, qn, icase, pos);
        if (at == SIZE_MAX) return;
        Span *sp = vec_push(spans);
        sp->start = at;
        sp->end = at + qn;
        pos = at + qn;
    }
}
