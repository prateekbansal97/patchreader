#ifndef PR_WORDDIFF_H
#define PR_WORDDIFF_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* A changed byte range [start, end) inside one line. */
typedef struct {
    size_t start, end;
} Span;

/* Lines longer than this are never compared word by word. */
#define WORDDIFF_MAX_LINE 4096

/* Compares two lines token by token (words, blanks, single punctuation
 * marks) and appends the changed byte ranges of a to sa and of b to sb, as
 * sorted, non-overlapping Spans (sa and sb must be Vecs of Span).
 * Returns false and appends nothing when the lines are too long or too
 * different for word marks to be useful: the whole row is changed then. */
bool word_diff(const char *a, size_t an, const char *b, size_t bn, Vec *sa, Vec *sb);

#endif