#ifndef PR_PATCH_H
#define PR_PATCH_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* Largest line number / line count accepted in a hunk header. */
#define PR_MAX_LINENO ((size_t)0x7fffffff)

typedef enum { LINE_CONTEXT, LINE_ADD, LINE_DEL } LineKind;

/* One hunk line. The text is a view into the source buffer given to
 * patch_parse: src + off, len bytes, without the leading ' ', '+' or '-'
 * and without the line ending. */
typedef struct {
    size_t off;
    size_t len;
    LineKind kind;
    bool no_eol; /* followed by "\ No newline at end of file" */
} Line;

typedef struct {
    size_t old_start, old_count;
    size_t new_start, new_count;
    size_t head_off, head_len; /* text after the closing "@@" (view into src) */
    Vec lines;                 /* Line */
} Hunk;

typedef enum {
    DIFF_MODIFIED,
    DIFF_ADDED,
    DIFF_DELETED,
    DIFF_RENAMED,
    DIFF_COPIED
} DiffStatus;

typedef struct {
    DiffStatus status;
    const char *old_path; /* NULL when the file is added */
    const char *new_path; /* NULL when the file is deleted */
    bool is_binary;
    bool git_;      /* parser-internal */
    bool saw_names; /* parser-internal */
    size_t adds, dels;
    Vec hunks; /* Hunk */
} FileDiff;

/* The source buffer must outlive the Patch. */
typedef struct {
    const char *src;
    size_t src_len;
    Arena arena; /* owns the path strings */
    Vec files;   /* FileDiff */
} Patch;

typedef struct {
    size_t line; /* 1-based, 0 when not tied to a line */
    char msg[128];
} ParseError;

/* Returns 0 on success, -1 on error (err, if non-NULL, is filled in).
 * patch_free must be called afterwards in both cases. */
int patch_parse(Patch *p, const char *src, size_t len, ParseError *err);
void patch_free(Patch *p);

const char *filediff_path(const FileDiff *f); /* path to show for a file */
char diffstatus_letter(DiffStatus s);         /* M A D R C */

#endif