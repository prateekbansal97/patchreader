#include "render.h"

#include <stdio.h>
#include <string.h>

#include "text.h"

typedef struct {
    const char *reset;
    const char *header, *bar, *hunk, *note;
    const char *ctx_gutter, *add_gutter, *del_gutter;
    const char *ctx_text, *add_text, *del_text;
} Theme;

static const Theme THEME_TRUE = {
    .reset = "\x1b[0m",
    .header = "\x1b[0;1;38;2;220;220;220;48;2;60;63;65m",
    .bar = "\x1b[0;7m",
    .hunk = "\x1b[0;38;2;125;150;205;48;2;43;48;66m",
    .note = "\x1b[0;2m",
    .ctx_gutter = "\x1b[0;38;2;120;120;120m",
    .add_gutter = "\x1b[0;38;2;120;150;125;48;2;36;68;46m",
    .del_gutter = "\x1b[0;38;2;160;125;125;48;2;82;38;42m",
    .ctx_text = "\x1b[0m",
    .add_text = "\x1b[0;38;2;220;220;220;48;2;36;68;46m",
    .del_text = "\x1b[0;38;2;220;220;220;48;2;82;38;42m",
};

static const Theme THEME_256 = {
    .reset = "\x1b[0m",
    .header = "\x1b[0;1;38;5;252;48;5;238m",
    .bar = "\x1b[0;7m",
    .hunk = "\x1b[0;38;5;110;48;5;236m",
    .note = "\x1b[0;2m",
    .ctx_gutter = "\x1b[0;38;5;244m",
    .add_gutter = "\x1b[0;38;5;108;48;5;22m",
    .del_gutter = "\x1b[0;38;5;138;48;5;52m",
    .ctx_text = "\x1b[0m",
    .add_text = "\x1b[0;38;5;252;48;5;22m",
    .del_text = "\x1b[0;38;5;252;48;5;52m",
};

size_t render_body_rows(int rows) {
    return rows > 2 ? (size_t)(rows - 2) : 0;
}

size_t render_max_top(const Model *m, int rows) {
    size_t h = render_body_rows(rows);
    return m->rows.len > h ? m->rows.len - h : 0;
}

size_t render_gutter_width(const Model *m) {
    return m->old_w + m->new_w + 3;
}

size_t render_max_left(const Model *m, int cols) {
    size_t gw = render_gutter_width(m);
    if (cols <= 0 || (size_t)cols <= gw) return 0;
    size_t text_cols = (size_t)cols - gw;
    return m->max_cols > text_cols ? m->max_cols - text_cols : 0;
}

static void pad(Buf *o, size_t n) {
    for (; n > 0; n--) buf_putc(o, ' ');
}

static void move_to(Buf *o, size_t row) {
    char s[32];
    int k = snprintf(s, sizeof s, "\x1b[%zu;1H", row);
    if (k > 0 && (size_t)k < sizeof s) buf_append(o, s, (size_t)k);
}

/* ASCII-only literal, clipped to cols; returns the columns used. */
static size_t put_ascii(Buf *o, const char *s, size_t cols) {
    size_t n = strlen(s);
    if (n > cols) n = cols;
    buf_append(o, s, n);
    return n;
}

static void draw_header(Buf *o, const Theme *t, const RenderView *v, size_t cols) {
    const FileDiff *f = v->model->file;
    char pre[64], post[64];
    snprintf(pre, sizeof pre, " %zu/%zu files   %c  ", v->file_idx + 1, v->file_count,
             diffstatus_letter(f->status));
    snprintf(post, sizeof post, "  +%zu -%zu ", f->adds, f->dels);

    buf_puts(o, t->header);
    size_t used = put_ascii(o, pre, cols);
    size_t rlen = strlen(post);
    size_t room = cols - used;
    size_t path_cols = room > rlen ? room - rlen : room;
    const char *path = filediff_path(f);
    used += text_window(o, path, strlen(path), 0, path_cols);
    if (cols >= used + rlen) {
        pad(o, cols - used - rlen);
        buf_puts(o, post);
    } else {
        pad(o, cols - used);
    }
    buf_puts(o, t->reset);
}

static void draw_bar(Buf *o, const Theme *t, const RenderView *v, size_t cols) {
    static const char left[] = " q quit   j/k scroll   [/] file   {/} hunk   h/l pan";
    size_t total = v->model->rows.len;
    size_t h = render_body_rows(v->rows);
    size_t bottom;
    if (!checked_add(v->top, h, &bottom) || bottom > total) bottom = total;
    int pct = total == 0 ? 100 : (int)(100.0 * (double)bottom / (double)total);

    char right[48];
    snprintf(right, sizeof right, " READ-ONLY  %d%% ", pct);
    size_t rlen = strlen(right);

    buf_puts(o, t->bar);
    if (cols > rlen) {
        size_t used = put_ascii(o, left, cols - rlen);
        pad(o, cols - rlen - used);
        buf_puts(o, right);
    } else {
        size_t used = put_ascii(o, right, cols);
        pad(o, cols - used);
    }
    buf_puts(o, t->reset);
}

static void draw_text_row(Buf *o, const Theme *t, const Row *r, size_t cols) {
    buf_puts(o, r->kind == ROW_HUNK ? t->hunk : t->note);
    size_t used = 0;
    if (cols > 1) {
        buf_putc(o, ' ');
        used = 1 + text_window(o, r->text, r->len, 0, cols - 1);
    }
    pad(o, cols - used);
    buf_puts(o, t->reset);
}

static void draw_diff_row(Buf *o, const Theme *t, const Model *m, const Row *r, size_t left,
                          size_t cols) {
    const char *gutter_style = t->ctx_gutter;
    const char *text_style = t->ctx_text;
    if (r->kind == ROW_ADD) {
        gutter_style = t->add_gutter;
        text_style = t->add_text;
    } else if (r->kind == ROW_DEL) {
        gutter_style = t->del_gutter;
        text_style = t->del_text;
    }

    char g[96];
    size_t pos = 0;
    int k;
    g[pos++] = ' ';
    if (r->old_no) k = snprintf(g + pos, sizeof g - pos, "%*zu", (int)m->old_w, r->old_no);
    else k = snprintf(g + pos, sizeof g - pos, "%*s", (int)m->old_w, "");
    if (k > 0) pos += (size_t)k;
    g[pos++] = ' ';
    if (r->new_no) k = snprintf(g + pos, sizeof g - pos, "%*zu", (int)m->new_w, r->new_no);
    else k = snprintf(g + pos, sizeof g - pos, "%*s", (int)m->new_w, "");
    if (k > 0) pos += (size_t)k;
    g[pos++] = ' ';
    g[pos] = '\0';

    buf_puts(o, gutter_style);
    size_t used = put_ascii(o, g, cols);
    if (used < cols) {
        size_t cw = cols - used;
        buf_puts(o, text_style);
        size_t shown = text_window(o, r->text, r->len, left, cw);
        if (r->no_eol && shown < cw) {
            buf_puts(o, gutter_style);
            shown += text_window(o, "  [no newline]", 14, 0, cw - shown);
        }
        pad(o, cw - shown);
    }
    buf_puts(o, t->reset);
}

void render_frame(Buf *out, const RenderView *v) {
    if (v->rows <= 0 || v->cols <= 0) return;
    const Theme *t = v->truecolor ? &THEME_TRUE : &THEME_256;
    size_t cols = (size_t)v->cols;

    move_to(out, 1);
    draw_header(out, t, v, cols);

    size_t h = render_body_rows(v->rows);
    for (size_t i = 0; i < h; i++) {
        move_to(out, i + 2);
        size_t idx;
        if (checked_add(v->top, i, &idx) && idx < v->model->rows.len) {
            const Row *r = vec_at(&v->model->rows, idx);
            if (r->kind == ROW_HUNK || r->kind == ROW_NOTE) draw_text_row(out, t, r, cols);
            else draw_diff_row(out, t, v->model, r, v->left, cols);
        } else {
            buf_puts(out, "\x1b[0m\x1b[2K");
        }
    }

    if (v->rows >= 2) {
        move_to(out, (size_t)v->rows);
        draw_bar(out, t, v, cols);
    }
}