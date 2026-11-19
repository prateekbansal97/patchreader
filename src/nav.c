#include "nav.h"

#include <stdint.h>
#include <string.h>

#include "render.h"

#define PAN_STEP 8

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
        if (r->kind == ROW_HUNK) return i;
    }
    return SIZE_MAX;
}

/* Index of the last hunk row before `top`, or SIZE_MAX. */
static size_t prev_hunk(const Model *m, size_t top) {
    size_t i = top < m->rows.len ? top : m->rows.len;
    while (i > 0) {
        i--;
        const Row *r = vec_at(&m->rows, i);
        if (r->kind == ROW_HUNK) return i;
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
    return true;
}

void nav_init(Nav *n, size_t file_count) {
    memset(n, 0, sizeof *n);
    n->file_count = file_count;
}

void nav_clamp(Nav *n, const Model *m, int rows, int cols) {
    size_t max_top = render_max_top(m, rows);
    size_t max_left = render_max_left(m, cols);
    if (n->top > max_top) n->top = max_top;
    if (n->left > max_left) n->left = max_left;
}

bool nav_key(Nav *n, Key k, const Model *m, int rows, int cols) {
    size_t body = render_body_rows(rows);
    size_t page = body > 1 ? body - 1 : 1;
    size_t half = body > 1 ? body / 2 : 1;
    size_t max_top = render_max_top(m, rows);
    size_t max_left = render_max_left(m, cols);
    size_t h;

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
        case ']': return step_file(n, +1);
        case '[': return step_file(n, -1);
        default: break;
        }
        break;
    default: break;
    }
    return false;
}