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

/* One line of a change block. */
typedef struct {
    const char *text;
    size_t len;
} BlockLine;

/* A changed byte range inside line `line` of a block. */
typedef struct {
    size_t line;
    Span span;
} BlockSpan;

/* Blocks with more lines than this are compared line by line instead. */
#define WORDDIFF_BLOCK_MAX_LINES 200

/* Compares a whole change block: lines[0..nd) are the removed lines and
 * lines[nd..nd+na) the added ones. Both sides are compared as one text (with
 * a line break between lines), so an inserted line shows up as a whole and a
 * changed line is matched with its real counterpart, wherever it sits.
 * Appends the changed ranges of every line to out (a Vec of BlockSpan),
 * sorted by line, then by position. Returns false and appends nothing when
 * the block is too large or too different for word marks to be useful. */
bool word_diff_block(const BlockLine *lines, size_t nd, size_t na, Vec *out);

#endif
