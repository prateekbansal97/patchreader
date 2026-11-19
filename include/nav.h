#ifndef PR_NAV_H
#define PR_NAV_H

#include <stdbool.h>
#include <stddef.h>

#include "input.h"
#include "model.h"

/* Viewer state: which file is shown and where the view sits in it. */
typedef struct {
    size_t file_count;
    size_t file; /* index of the file shown */
    size_t top;  /* first visible model row */
    size_t left; /* first visible content column */
    bool quit;
} Nav;

void nav_init(Nav *n, size_t file_count);

/* Pulls top/left back into range for the current model and screen size. */
void nav_clamp(Nav *n, const Model *m, int rows, int cols);

/* Applies one key to the state; m is the model of the current file.
 * Returns true when the shown file changed: the caller must then rebuild
 * the model for n->file (top and left are already reset to 0). */
bool nav_key(Nav *n, Key k, const Model *m, int rows, int cols);

#endif