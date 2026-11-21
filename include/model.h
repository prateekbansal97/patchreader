#ifndef PR_MODEL_H
#define PR_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#include "patch.h"
#include "util.h"

typedef enum { ROW_HUNK, ROW_CONTEXT, ROW_ADD, ROW_DEL, ROW_NOTE } RowKind;

/* One screen row of a file's diff. text points into the patch source (or
 * the model's arena for hunk and note rows) and is raw: pass it through
 * text_window before drawing. */
typedef struct {
    RowKind kind;
    size_t old_no; /* 0 = no old line number */
    size_t new_no; /* 0 = no new line number */
    const char *text;
    size_t len;
    bool no_eol;
    bool has_pair;  /* a removed row followed by an added row (or the reverse) */
    size_t pair;    /* index of the partner row when has_pair */
} Row;

typedef struct {
    const FileDiff *file;
    Arena arena; /* owns generated hunk-header and note text */
    Vec rows;    /* Row */
    size_t old_w, new_w; /* digits needed by the line-number gutters */
    size_t max_cols;     /* widest content line, in display columns */
} Model;

/* The patch (and its source buffer) must outlive the model. */
void model_build(Model *m, const Patch *p, const FileDiff *f);
void model_free(Model *m);

size_t num_digits(size_t n);

#endif