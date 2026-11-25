#include "nav.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "render.h"
#include "search.h"
#include "text.h"

#define PAN_STEP 8

_Static_assert(HISTORY_TEXT <= NAV_TEXT_MAX, "a history entry must fit the search prompt");

static size_t scroll_down(size_t pos, size_t n, size_t max) {
    if (pos >= max) return max;
    return n > max - pos ? max : pos + n;
}

static size_t scroll_up(size_t pos, size_t n) {
    return pos > n ? pos - n : 0;
}

/* Index of the first hunk row after `top`, or SIZE_MAX. */
static size_t next_hunk(const Model *m, size_t top) {
    for (size_t i = top; i < m->rows.len; i++) {
        if (i == top) continue;
        const Row *r = vec_at(&m->rows, i);
        if (r->kind == ROW_WAVE) return i;
    }
    return SIZE_MAX;
}

/* Index of the last hunk row before `top`, or SIZE_MAX. */
static size_t prev_hunk(const Model *m, size_t top) {
    size_t i = top < m->rows.len ? top : m->rows.len;
    while (i > 0) {
        i--;
        const Row *r = vec_at(&m->rows, i);
        if (r->kind == ROW_WAVE) return i;
    }
    return SIZE_MAX;
}

static bool step_file(Nav *n, int dir) {
    if (dir > 0 && n->file + 1 < n->file_count) {
        n->file++;
    } else if (dir < 0 && n->file > 0) {
        n->file--;
    } else {
        return false;
    }
    n->top = 0;
    n->left = 0;
    n->hit = SIZE_MAX;
    n->status[0] = '\0';
    return true;
}

void nav_init(Nav *n, size_t file_count) {
    memset(n, 0, sizeof *n);
    n->file_count = file_count;
    n->hit = SIZE_MAX;
    n->hist_pos = SIZE_MAX;
}

static bool row_matches(const Nav *n, const Row *r, bool icase) {
    return search_find(r->text, r->len, n->query, n->query_len, icase, 0) != SIZE_MAX;
}

/* Moves to the next (dir > 0) or previous match of the active query, wrapping
 * around at the ends. A search of all files goes through every file of the
 * patch in order and may land in another file: the result is then true, and
 * the caller must rebuild the model (top is already near the match, nav_clamp
 * fits it to the new file). */
static bool search_step(Nav *n, const Model *m, int dir, int rows) {
    if (n->query_len == 0) {
        snprintf(n->status, sizeof n->status, "No search pattern");
        return false;
    }
    bool icase = search_icase(n->query, n->query_len);
    bool all = n->scope_all && n->patch != NULL && n->patch->files.len > 1;
    size_t nfiles = all ? n->patch->files.len : 1;
    bool have = n->hit != SIZE_MAX && n->hit < m->rows.len;
    size_t from_next = have ? n->hit + 1 : n->top;
    size_t from_prev = have ? n->hit : n->top;

    /* Matches are ordered by (file, row); the search starts at (n->file, from_*). */
    size_t first_f = SIZE_MAX, first_r = SIZE_MAX, last_f = SIZE_MAX, last_r = SIZE_MAX;
    size_t next_f = SIZE_MAX, next_r = SIZE_MAX, next_ord = 0;
    size_t prev_f = SIZE_MAX, prev_r = SIZE_MAX, prev_ord = 0;
    size_t total = 0;
    for (size_t k = 0; k < nfiles; k++) {
        size_t f = all ? k : n->file;
        Model other;
        const Model *mm = m;
        bool built = f != n->file;
        if (built) {
            model_build(&other, n->patch, vec_at(&n->patch->files, f));
            mm = &other;
        }
        for (size_t i = 0; i < mm->rows.len; i++) {
            if (!row_matches(n, vec_at(&mm->rows, i), icase)) continue;
            total++;
            if (first_f == SIZE_MAX) {
                first_f = f;
                first_r = i;
            }
            last_f = f;
            last_r = i;
            if (next_f == SIZE_MAX && (f > n->file || (f == n->file && i >= from_next))) {
                next_f = f;
                next_r = i;
                next_ord = total;
            }
            if (f < n->file || (f == n->file && i < from_prev)) {
                prev_f = f;
                prev_r = i;
                prev_ord = total;
            }
        }
        if (built) model_free(&other);
    }
    if (total == 0) {
        n->hit = SIZE_MAX;
        snprintf(n->status, sizeof n->status, "Not found: %s", n->query);
        return false;
    }

    bool wrapped = false;
    size_t found_f, found_r, ordinal;
    if (dir > 0) {
        if (next_f != SIZE_MAX) {
            found_f = next_f;
            found_r = next_r;
            ordinal = next_ord;
        } else {
            found_f = first_f;
            found_r = first_r;
            ordinal = 1;
            wrapped = true;
        }
    } else {
        if (prev_f != SIZE_MAX) {
            found_f = prev_f;
            found_r = prev_r;
            ordinal = prev_ord;
        } else {
            found_f = last_f;
            found_r = last_r;
            ordinal = total;
            wrapped = true;
        }
    }

    bool changed = found_f != n->file;
    size_t body = render_body_rows(rows);
    n->hit = found_r;
    if (changed) {
        n->file = found_f;
        n->left = 0;
        n->top = found_r > body / 3 ? found_r - body / 3 : 0;
    } else {
        size_t end;
        if (found_r < n->top || !checked_add(n->top, body, &end) || found_r >= end) {
            n->top = found_r > body / 3 ? found_r - body / 3 : 0;
            size_t max_top = render_max_top(m, rows);
            if (n->top > max_top) n->top = max_top;
        }
    }
    if (all)
        snprintf(n->status, sizeof n->status, "/%s  %zu/%zu  %.48s%s", n->query, ordinal, total,
                 filediff_path(vec_at(&n->patch->files, found_f)), wrapped ? "  (wrapped)" : "");
    else
        snprintf(n->status, sizeof n->status, "/%s  %zu/%zu%s", n->query, ordinal, total,
                 wrapped ? "  (wrapped)" : "");
    return changed;
}

static void history_show(Nav *n, const char *s, size_t len) {
    memcpy(n->input, s, len);
    n->input[len] = '\0';
    n->input_len = len;
}

/* Up (dir < 0) and Down (dir > 0) in the search prompt: older and newer
 * searches. Going down past the newest brings back what was being typed. */
static void history_step(Nav *n, int dir) {
    const History *h = n->hist;
    if (h == NULL || h->count == 0) return;
    if (n->hist_pos != SIZE_MAX && n->hist_pos >= h->count) n->hist_pos = SIZE_MAX;
    if (n->hist_pos == SIZE_MAX) {
        if (dir > 0) return;
        memcpy(n->draft, n->input, n->input_len + 1);
        n->draft_len = n->input_len;
        n->hist_pos = h->count - 1;
    } else if (dir < 0) {
        if (n->hist_pos > 0) n->hist_pos--;
    } else if (n->hist_pos + 1 < h->count) {
        n->hist_pos++;
    } else {
        n->hist_pos = SIZE_MAX;
        history_show(n, n->draft, n->draft_len);
        return;
    }
    history_show(n, h->items[n->hist_pos], strlen(h->items[n->hist_pos]));
}

static bool search_key(Nav *n, Key k, const Model *m, int rows) {
    switch (k.type) {
    case KEY_EOF: n->quit = true; break;
    case KEY_ESC: n->mode = NAV_VIEW; break;
    case KEY_ENTER:
        n->mode = NAV_VIEW;
        if (n->input_len > 0) {
            memcpy(n->query, n->input, n->input_len + 1);
            n->query_len = n->input_len;
            n->hit = SIZE_MAX;
            if (n->hist != NULL && history_add(n->hist, n->query, n->query_len)) n->hist_dirty = true; 
        }
        n->scope_all = n->typing_all;
        return search_step(n, m, +1, rows);
    case KEY_UP: history_step(n, -1); break;
    case KEY_DOWN: history_step(n, +1); break;
    case KEY_BACKSPACE:
        if (n->input_len == 0) {
            n->mode = NAV_VIEW;
            break;
        }
        do {
            n->input_len--;
        } while (n->input_len > 0 && ((unsigned char)n->input[n->input_len] & 0xC0u) == 0x80u);
        n->input[n->input_len] = '\0';
        break;
    case KEY_CTRL:
        if (k.ch == 'u') {
            n->input_len = 0;
            n->input[0] = '\0';
        }
        break;
    case KEY_CHAR:
        if (k.ch >= 0x20 && k.ch != 0x7F && !(k.ch >= 0x80 && k.ch < 0xA0)) {
            Buf b;
            buf_init(&b);
            utf8_encode(&b, k.ch);
            if (n->input_len + b.len < sizeof n->input) {
                memcpy(n->input + n->input_len, b.data, b.len);
                n->input_len += b.len;
                n->input[n->input_len] = '\0';
            }
            buf_free(&b);
        }
        break;
    default: break;
    }
    return false;
}

static void pick_fit(Nav *n, int rows) {
    size_t body = render_body_rows(rows);
    if (n->file_count > 0 && n->pick >= n->file_count) n->pick = n->file_count - 1;
    if (n->pick < n->pick_top) n->pick_top = n->pick;
    if (body > 0 && n->pick - n->pick_top >= body) n->pick_top = n->pick + 1 - body;
    if (body == 0) n->pick_top = n->pick;
}

static bool pick_key(Nav *n, Key k, int rows) {
    size_t body = render_body_rows(rows);
    size_t page = body > 1 ? body - 1 : 1;
    size_t last = n->file_count > 0 ? n->file_count - 1 : 0;
    switch (k.type) {
    case KEY_EOF: n->quit = true; break;
    case KEY_ESC: n->mode = NAV_VIEW; break;
    case KEY_DOWN: n->pick = scroll_down(n->pick, 1, last); break;
    case KEY_UP: n->pick = scroll_up(n->pick, 1); break;
    case KEY_PGDN: n->pick = scroll_down(n->pick, page, last); break;
    case KEY_PGUP: n->pick = scroll_up(n->pick, page); break;
    case KEY_HOME: n->pick = 0; break;
    case KEY_END: n->pick = last; break;
    case KEY_ENTER:
        n->mode = NAV_VIEW;
        if (n->pick != n->file && n->pick < n->file_count) {
            n->file = n->pick;
            n->top = 0;
            n->left = 0;
            n->hit = SIZE_MAX;
            n->status[0] = '\0';
            return true;
        }
        break;
    case KEY_CHAR:
        switch (k.ch) {
        case 'q':
        case 'f': n->mode = NAV_VIEW; break;
        case 'j': n->pick = scroll_down(n->pick, 1, last); break;
        case 'k': n->pick = scroll_up(n->pick, 1); break;
        case ' ': n->pick = scroll_down(n->pick, page, last); break;
        case 'b': n->pick = scroll_up(n->pick, page); break;
        case 'g': n->pick = 0; break;
        case 'G': n->pick = last; break;
        default: break;
        }
        break;
    default: break;
    }
    pick_fit(n, rows);
    return false;
}

void nav_clamp(Nav *n, const Model *m, int rows, int cols) {
    size_t max_top = render_max_top(m, rows);
    size_t max_left = render_max_left(m, cols);
    if (n->top > max_top) n->top = max_top;
    if (n->left > max_left) n->left = max_left;
    pick_fit(n, rows);
}

bool nav_key(Nav *n, Key k, const Model *m, int rows, int cols) {
    size_t body = render_body_rows(rows);
    size_t page = body > 1 ? body - 1 : 1;
    size_t half = body > 1 ? body / 2 : 1;
    size_t max_top = render_max_top(m, rows);
    size_t max_left = render_max_left(m, cols);
    size_t h;

    if (n->mode == NAV_PICK) return pick_key(n, k, rows);
    if (n->mode == NAV_SEARCH) return search_key(n, k, m, rows);
    if (n->mode == NAV_HELP) {
        if (k.type == KEY_EOF) n->quit = true;
        else if (k.type != KEY_RESIZE && k.type != KEY_NONE) n->mode = NAV_VIEW;
        return false;
    }
    if (k.type != KEY_RESIZE) {
        bool search_key_pressed = k.type == KEY_CHAR && (k.ch == 'n' || k.ch == 'N');
        if (!search_key_pressed) n->status[0] = '\0';
    }

    switch (k.type) {
    case KEY_EOF: n->quit = true; break;
    case KEY_DOWN:
    case KEY_ENTER: n->top = scroll_down(n->top, 1, max_top); break;
    case KEY_UP: n->top = scroll_up(n->top, 1); break;
    case KEY_PGDN: n->top = scroll_down(n->top, page, max_top); break;
    case KEY_PGUP: n->top = scroll_up(n->top, page); break;
    case KEY_HOME: n->top = 0; break;
    case KEY_END: n->top = max_top; break;
    case KEY_RIGHT: n->left = scroll_down(n->left, PAN_STEP, max_left); break;
    case KEY_LEFT: n->left = scroll_up(n->left, PAN_STEP); break;
    case KEY_TAB: return step_file(n, +1);
    case KEY_BTAB: return step_file(n, -1);
    case KEY_CTRL:
        if (k.ch == 'd') n->top = scroll_down(n->top, half, max_top);
        else if (k.ch == 'u') n->top = scroll_up(n->top, half);
        break;
    case KEY_CHAR:
        switch (k.ch) {
        case 'q': n->quit = true; break;
        case 'j': n->top = scroll_down(n->top, 1, max_top); break;
        case 'k': n->top = scroll_up(n->top, 1); break;
        case ' ': n->top = scroll_down(n->top, page, max_top); break;
        case 'b': n->top = scroll_up(n->top, page); break;
        case 'g': n->top = 0; break;
        case 'G': n->top = max_top; break;
        case 'l': n->left = scroll_down(n->left, PAN_STEP, max_left); break;
        case 'h': n->left = scroll_up(n->left, PAN_STEP); break;
        case '0': n->left = 0; break;
        case '$': n->left = max_left; break;
        case '}':
        case 'J':
            h = next_hunk(m, n->top);
            if (h != SIZE_MAX) n->top = h > max_top ? max_top : h;
            break;
        case '{':
        case 'K':
            h = prev_hunk(m, n->top);
            if (h != SIZE_MAX) n->top = h;
            break;
        case 'f':
            n->mode = NAV_PICK;
            n->pick = n->file;
            pick_fit(n, rows);
            break;
        case '?': n->mode = NAV_HELP; break;
        case '/':
            n->mode = NAV_SEARCH;
            n->input_len = 0;
            n->input[0] = '\0';
            n->hist_pos = SIZE_MAX;
            n->typing_all = false;
            break;
        case 'F':
            n->mode = NAV_SEARCH;
            n->input_len = 0;
            n->input[0] = '\0';
            n->hist_pos = SIZE_MAX;
            n->typing_all = true;
            break;
        case 'n': return search_step(n, m, +1, rows);
        case 'N': return search_step(n, m, -1, rows);
        case ']': return step_file(n, +1);
        case '[': return step_file(n, -1);
        default: break;
        }
        break;
    default: break;
    }
    return false;
}
