#ifndef PR_NAV_H
#define PR_NAV_H

#include <stdbool.h>
#include <stddef.h>

#include "input.h"
#include "model.h"

typedef enum {
    NAV_VIEW,   /* reading the diff */
    NAV_PICK,   /* choosing a file from the list */
    NAV_HELP,   /* help page */
    NAV_SEARCH  /* typing a search query */
} NavMode;

#define NAV_TEXT_MAX 96

/* Viewer state: which file is shown and where the view sits in it. */
typedef struct {
    size_t file_count;
    size_t file; /* index of the file shown */
    size_t top;  /* first visible model row */
    size_t left; /* first visible content column */
    bool quit;
    NavMode mode;
    size_t pick, pick_top;         /* file list selection and first visible entry */
    char query[NAV_TEXT_MAX];      /* active search, NUL-terminated UTF-8 */
    size_t query_len;
    char input[NAV_TEXT_MAX];      /* query being typed */
    size_t input_len;
    size_t hit;                    /* row of the current match, SIZE_MAX if none */
    char status[NAV_TEXT_MAX + 32]; /* message for the bottom bar, "" if none */
} Nav;

void nav_init(Nav *n, size_t file_count);

/* Pulls top/left back into range for the current model and screen size. */
void nav_clamp(Nav *n, const Model *m, int rows, int cols);

/* Applies one key to the state; m is the model of the current file.
 * Returns true when the shown file changed: the caller must then rebuild
 * the model for n->file (top and left are already reset to 0). */
bool nav_key(Nav *n, Key k, const Model *m, int rows, int cols);

#endif