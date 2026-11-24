#ifndef PR_HISTORY_H
#define PR_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#define HISTORY_MAX 100                      /* searches kept */
#define HISTORY_TEXT 96                      /* longest search + 1 (NAV_TEXT_MAX) */
#define HISTORY_FILE_MAX ((size_t)64 * 1024) /* larger history files are ignored */

/* Earlier search queries, oldest first. */
typedef struct {
    char items[HISTORY_MAX][HISTORY_TEXT];
    size_t count;
} History;

void history_init(History *h);

/* Makes s the newest entry. An equal older entry is dropped first, and the
 * oldest entry is dropped when the list is full. Returns false (and changes
 * nothing) for an empty entry, one that does not fit, or one with control
 * bytes. */
bool history_add(History *h, const char *s, size_t len);

/* "$HOME/.patchreader_history". False when HOME is not an absolute path or
 * the result does not fit in cap bytes. */
bool history_path(char *out, size_t cap);

/* Adds the lines of the file at path. A missing, unreadable or oversized
 * file adds nothing; lines that are not valid entries are skipped. */
void history_load(History *h, const char *path);

/* Writes the entries to path through a private (0600) temporary file that
 * replaces it with one rename, so a crash never leaves a half-written
 * history. Returns false on any error. This is the only place that writes a
 * file. */
bool history_save(const History *h, const char *path);

#endif
