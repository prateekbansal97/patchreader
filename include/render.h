#ifndef PR_RENDER_H
#define PR_RENDER_H

#include <stdbool.h>
#include <stddef.h>

#include "model.h"
#include "util.h"

typedef struct {
    const Model *model;
    size_t file_idx;   /* 0-based index of the file shown */
    size_t file_count;
    size_t top;        /* first visible model row */
    size_t left;       /* first visible content column */
    int rows, cols;    /* screen size in character cells */
    bool truecolor;    /* 24-bit colors, otherwise the 256-color palette */
} RenderView;

/* Rows available for diff content (the screen minus header and bottom bar). */
size_t render_body_rows(int rows);

/* Largest useful value of RenderView.top. */
size_t render_max_top(const Model *m, int rows);

/* Columns taken by the line-number gutter. */
size_t render_gutter_width(const Model *m);

/* Largest useful value of RenderView.left for a screen `cols` wide. */
size_t render_max_left(const Model *m, int cols);

/* Appends one complete frame (every screen row is repainted, so no clear is
 * needed between frames). All text from the patch goes through text_window,
 * so the output contains no escape sequences other than our own. */
void render_frame(Buf *out, const RenderView *v);

#endif