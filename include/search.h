#ifndef PR_SEARCH_H
#define PR_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* At most this many marks are produced for one row. */
#define SEARCH_MAX_SPANS 256

/* Smart case: a query without capital letters ignores ASCII case. */
bool search_icase(const char *q, size_t qn);

/* Byte offset of the first occurrence of q in s at or after `from`, or
 * SIZE_MAX. An empty query never matches. */
size_t search_find(const char *s, size_t n, const char *q, size_t qn, bool icase, size_t from);

/* Appends the non-overlapping occurrences of q in s to spans (a Vec of
 * Span from worddiff.h), in order. */
void search_spans(const char *s, size_t n, const char *q, size_t qn, bool icase, Vec *spans);

#endif
