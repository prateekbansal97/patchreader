#include "model.h"

#include <stdio.h>
#include <string.h>

#include "text.h"

size_t num_digits(size_t n) {
    size_t d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void set_text(Model *m, Row *r, const char *s, size_t n) {
    r->text = arena_strndup(&m->arena, s, n);
    r->len = n;
}

static void hunk_row(Model *m, const Patch *p, const Hunk *h) {
    char nums[128];
    int k = snprintf(nums, sizeof nums, "@@ -%zu,%zu +%zu,%zu @@", h->old_start, h->old_count,
                     h->new_start, h->new_count);
    if (k < 0) k = 0;
    if ((size_t)k >= sizeof nums) k = (int)sizeof nums - 1;

    Buf b;
    buf_init(&b);
    buf_append(&b, nums, (size_t)k);
    if (h->head_len > 0) {
        buf_putc(&b, ' ');
        buf_append(&b, p->src + h->head_off, h->head_len);
    }
    Row *r = vec_push(&m->rows);
    r->kind = ROW_HUNK;
    set_text(m, r, b.data, b.len);
    buf_free(&b);
}

static void note_row(Model *m, const char *s) {
    Row *r = vec_push(&m->rows);
    r->kind = ROW_NOTE;
    set_text(m, r, s, strlen(s));
}

static void empty_file_note(Model *m, const FileDiff *f) {
    if (f->is_binary) {
        note_row(m, "Binary file not shown");
    } else if ((f->status == DIFF_RENAMED || f->status == DIFF_COPIED) && f->old_path) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, f->status == DIFF_RENAMED ? "Renamed from " : "Copied from ");
        buf_puts(&b, f->old_path);
        note_row(m, b.data);
        buf_free(&b);
    } else if (f->status == DIFF_ADDED) {
        note_row(m, "Empty file added");
    } else if (f->status == DIFF_DELETED) {
        note_row(m, "Empty file deleted");
    } else {
        note_row(m, "No content changes");
    }
}

/* Pairs the removed and added rows of each change block one to one, in
 * order: the first removed row with the first added row, and so on. */
static void pair_rows(Model *m, size_t first, size_t end) {
    size_t i = first;
    while (i < end) {
        if (((const Row *)vec_at(&m->rows, i))->kind != ROW_DEL) {
            i++;
            continue;
        }
        size_t d0 = i;
        while (i < end && ((const Row *)vec_at(&m->rows, i))->kind == ROW_DEL) i++;
        size_t d1 = i;
        size_t a0 = i;
        while (i < end && ((const Row *)vec_at(&m->rows, i))->kind == ROW_ADD) i++;
        size_t a1 = i;
        size_t n = d1 - d0 < a1 - a0 ? d1 - d0 : a1 - a0;
        for (size_t k = 0; k < n; k++) {
            Row *d = vec_at(&m->rows, d0 + k);
            Row *a = vec_at(&m->rows, a0 + k);
            d->has_pair = a->has_pair = true;
            d->pair = a0 + k;
            a->pair = d0 + k;
        }
    }
}

void model_build(Model *m, const Patch *p, const FileDiff *f) {
    memset(m, 0, sizeof *m);
    m->file = f;
    arena_init(&m->arena);
    vec_init(&m->rows, sizeof(Row));

    size_t max_old = 0, max_new = 0;
    for (size_t i = 0; i < f->hunks.len; i++) {
        const Hunk *h = vec_at(&f->hunks, i);
        hunk_row(m, p, h);
        size_t first_line_row = m->rows.len;

        size_t old_no = h->old_start, new_no = h->new_start;
        for (size_t j = 0; j < h->lines.len; j++) {
            const Line *l = vec_at(&h->lines, j);
            Row *r = vec_push(&m->rows);
            r->text = p->src + l->off;
            r->len = l->len;
            r->no_eol = l->no_eol;
            switch (l->kind) {
            case LINE_CONTEXT:
                r->kind = ROW_CONTEXT;
                r->old_no = old_no++;
                r->new_no = new_no++;
                break;
            case LINE_ADD:
                r->kind = ROW_ADD;
                r->new_no = new_no++;
                break;
            case LINE_DEL:
                r->kind = ROW_DEL;
                r->old_no = old_no++;
                break;
            }
            if (r->old_no > max_old) max_old = r->old_no;
            if (r->new_no > max_new) max_new = r->new_no;
            size_t w = text_width(r->text, r->len);
            if (w > m->max_cols) m->max_cols = w;
        }
        pair_rows(m, first_line_row, m->rows.len);
    }
    if (m->rows.len == 0) empty_file_note(m, f);

    m->old_w = num_digits(max_old);
    m->new_w = num_digits(max_new);
}

void model_free(Model *m) {
    vec_free(&m->rows);
    arena_free(&m->arena);
    memset(m, 0, sizeof *m);
}