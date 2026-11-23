#include "render.h"

#include <stdio.h>
#include <string.h>

#include "search.h"
#include "text.h"
#include "worddiff.h"

typedef struct {
    const char *reset;
    const char *header, *bar, *hunk, *note;
    const char *ctx_gutter, *add_gutter, *del_gutter;
    const char *ctx_text, *add_text, *del_text;
    const char *add_word, *del_word; /* changed words inside a row */
    const char *match;               /* search matches */
    const char *sel;                 /* selected entry in the file list */
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
    .add_word = "\x1b[0;1;38;2;240;240;240;48;2;34;112;64m",
    .del_word = "\x1b[0;1;38;2;240;240;240;48;2;140;48;56m",
    .match = "\x1b[0;30;48;2;214;178;62m",
    .sel = "\x1b[0;1;38;2;240;240;240;48;2;70;90;130m",
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
    .add_word = "\x1b[0;1;38;5;255;48;5;28m",
    .del_word = "\x1b[0;1;38;5;255;48;5;88m",
    .match = "\x1b[0;38;5;16;48;5;178m",
    .sel = "\x1b[0;1;38;5;255;48;5;24m",
};

/* No colors: rows are told apart by a sign in the gutter, marks by reverse video. */
static const Theme THEME_MONO = {
    .reset = "\x1b[0m",
    .header = "\x1b[0;1;7m",
    .bar = "\x1b[0;7m",
    .hunk = "\x1b[0;1m",
    .note = "\x1b[0;2m",
    .ctx_gutter = "\x1b[0;2m",
    .add_gutter = "\x1b[0;2m",
    .del_gutter = "\x1b[0;2m",
    .ctx_text = "\x1b[0m",
    .add_text = "\x1b[0m",
    .del_text = "\x1b[0m",
    .add_word = "\x1b[0;7m",
    .del_word = "\x1b[0;7m",
    .match = "\x1b[0;1;7m",
    .sel = "\x1b[0;7m",
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

/* " <pre> <path> ...... <post> " on one line, the path clipped to fit. */
static void path_line(Buf *o, const char *pre, const char *path, const char *post, size_t cols) {
    size_t used = put_ascii(o, pre, cols);
    size_t rlen = strlen(post);
    size_t room = cols - used;
    size_t path_cols = room > rlen ? room - rlen : room;
    used += text_window(o, path, strlen(path), 0, path_cols);
    if (cols >= used + rlen) {
        pad(o, cols - used - rlen);
        buf_puts(o, post);
    } else {
        pad(o, cols - used);
    }
}

static void draw_header(Buf *o, const Theme *t, const RenderView *v, size_t cols) {
    buf_puts(o, t->header);
    if (v->help || (v->picker && v->patch)) {
        char s[64];
        if (v->help) snprintf(s, sizeof s, " Help");
        else snprintf(s, sizeof s, " Files (%zu)", v->patch->files.len);
        size_t used = put_ascii(o, s, cols);
        pad(o, cols - used);
    } else {
        const FileDiff *f = v->model->file;
        char pre[64], post[64];
        snprintf(pre, sizeof pre, " %zu/%zu files   %c  ", v->file_idx + 1, v->file_count,
                 diffstatus_letter(f->status));
        snprintf(post, sizeof post, "  +%zu -%zu ", f->adds, f->dels);
        path_line(o, pre, filediff_path(f), post, cols);
    }
    buf_puts(o, t->reset);
}

static void draw_bar(Buf *o, const Theme *t, const RenderView *v, size_t cols) {
    size_t total = v->model->rows.len;
    size_t h = render_body_rows(v->rows);
    size_t bottom;
    if (!checked_add(v->top, h, &bottom) || bottom > total) bottom = total;
    int pct = total == 0 ? 100 : (int)(100.0 * (double)bottom / (double)total);

    char right[48];
    if (v->help || v->picker) snprintf(right, sizeof right, " READ-ONLY ");
    else snprintf(right, sizeof right, " READ-ONLY  %d%% ", pct);
    size_t rlen = strlen(right);

    Buf left;
    buf_init(&left);
    if (v->typing) {
        buf_putc(&left, '/');
        buf_append(&left, v->input ? v->input : "", v->input ? v->input_len : 0);
        buf_putc(&left, '_');
    } else if (v->help) {
        buf_puts(&left, " any key closes the help");
    } else if (v->picker) {
        buf_puts(&left, " j/k move   Enter open   Esc close");
    } else if (v->status && v->status[0]) {
        buf_putc(&left, ' ');
        buf_puts(&left, v->status);
    } else {
        buf_puts(&left, " q quit  j/k scroll  [/] file  f files  / find  ? help");
    }

    buf_puts(o, t->bar);
    if (cols > rlen) {
        size_t used = text_window(o, left.data, left.len, 0, cols - rlen);
        pad(o, cols - rlen - used);
        buf_puts(o, right);
    } else {
        size_t used = put_ascii(o, right, cols);
        pad(o, cols - used);
    }
    buf_puts(o, t->reset);
    buf_free(&left);
}

/* The changed words of an added or removed row, found against its partner
 * row. Returns false when the whole row should be drawn plainly. */
static bool word_spans(const Model *m, const Row *r, Vec *spans) {
    if (!r->has_pair) return false;
    const Row *p = vec_at(&m->rows, r->pair);
    Vec other;
    vec_init(&other, sizeof(Span));
    bool ok;
    if (r->kind == ROW_DEL) ok = word_diff(r->text, r->len, p->text, p->len, spans, &other);
    else ok = word_diff(p->text, p->len, r->text, r->len, &other, spans);
    vec_free(&other);
    return ok && spans->len > 0;
}

/* Draws the part of text columns [c0, c1) that falls inside the visible
 * window [left, left + cw); returns the columns written. */
static size_t draw_part(Buf *o, const char *style, const Row *r, size_t c0, size_t c1, size_t left,
                        size_t cw) {
    size_t right;
    if (!checked_add(left, cw, &right)) right = SIZE_MAX;
    size_t s = c0 > left ? c0 : left;
    size_t e = c1 < right ? c1 : right;
    if (s >= e) return 0;
    buf_puts(o, style);
    return text_window(o, r->text, r->len, s, e - s);
}

/* Row text in segments: plain parts in text_style, changed words in word_style. */
static size_t draw_words(Buf *o, const Row *r, const Vec *spans, const char *text_style,
                         const char *word_style, size_t left, size_t cw) {
    size_t shown = 0, pos = 0, col = 0;
    for (size_t i = 0; i < spans->len; i++) {
        const Span *sp = vec_at(spans, i);
        if (sp->start < pos || sp->end > r->len || sp->end < sp->start) continue;
        if (sp->start > pos) {
            size_t c1 = text_width(r->text, sp->start);
            shown += draw_part(o, text_style, r, col, c1, left, cw);
            col = c1;
        }
        size_t c1 = text_width(r->text, sp->end);
        shown += draw_part(o, word_style, r, col, c1, left, cw);
        col = c1;
        pos = sp->end;
    }
    if (pos < r->len) shown += draw_part(o, text_style, r, col, text_width(r->text, r->len), left, cw);
    return shown;
}

/* Occurrences of the active search query in a row. */
static bool search_marks(const RenderView *v, const Row *r, Vec *spans) {
    if (!v->query || v->query_len == 0) return false;
    search_spans(r->text, r->len, v->query, v->query_len, search_icase(v->query, v->query_len),
                 spans);
    return spans->len > 0;
}

static void draw_text_row(Buf *o, const Theme *t, const RenderView *v, const Row *r,
                          size_t cols) {
    const char *style = r->kind == ROW_HUNK ? t->hunk : t->note;
    buf_puts(o, style);
    size_t used = 0;
    if (cols > 1) {
        buf_putc(o, ' ');
        Vec spans;
        vec_init(&spans, sizeof(Span));
        if (search_marks(v, r, &spans)) used = 1 + draw_words(o, r, &spans, style, t->match, 0, cols - 1);
        else used = 1 + text_window(o, r->text, r->len, 0, cols - 1);
        vec_free(&spans);
        buf_puts(o, style);
    }
    pad(o, cols - used);
    buf_puts(o, t->reset);
}

static void draw_diff_row(Buf *o, const Theme *t, const RenderView *v, const Row *r, size_t cols) {
    const Model *m = v->model;
    size_t left = v->left;
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
    /* without colors a sign marks added and removed rows */
    g[pos++] = (char)(v->mono ? (r->kind == ROW_ADD ? '+' : r->kind == ROW_DEL ? '-' : ' ') : ' ');
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
        size_t shown;
        Vec spans;
        vec_init(&spans, sizeof(Span));
        if (search_marks(v, r, &spans)) {
            shown = draw_words(o, r, &spans, text_style, t->match, left, cw);
        } else if (word_spans(m, r, &spans)) {
            shown = draw_words(o, r, &spans, text_style,
                               r->kind == ROW_ADD ? t->add_word : t->del_word, left, cw);
        } else {
            buf_puts(o, text_style);
            shown = text_window(o, r->text, r->len, left, cw);
        }
        vec_free(&spans);
        if (shown < cw) buf_puts(o, text_style);
        if (r->no_eol && shown < cw) {
            buf_puts(o, gutter_style);
            shown += text_window(o, "  [no newline]", 14, 0, cw - shown);
        }
        pad(o, cw - shown);
    }
    buf_puts(o, t->reset);
}

static const char *const HELP[] = {
    " Moving",
    "   j / k, Down / Up        scroll one line",
    "   Space / b, PgDn / PgUp  scroll one page",
    "   Ctrl-d / Ctrl-u         scroll half a page",
    "   g / G, Home / End       top / bottom",
    "   h / l, Left / Right     pan sideways      0 / $  line start / end",
    "",
    " Changes and files",
    "   } / {   (J / K)         next / previous hunk",
    "   ] / [   (Tab / S-Tab)   next / previous file",
    "   f                       file list",
    "",
    " Search",
    "   /                       search the current file",
    "   n / N                   next / previous match",
    "",
    " Other",
    "   ?                       this help",
    "   q                       quit",
};

static void draw_help_row(Buf *o, const Theme *t, size_t i, size_t cols) {
    buf_puts(o, t->ctx_text);
    size_t used = 0;
    if (i < sizeof HELP / sizeof HELP[0]) used = put_ascii(o, HELP[i], cols);
    pad(o, cols - used);
    buf_puts(o, t->reset);
}

static void draw_pick_row(Buf *o, const Theme *t, const RenderView *v, size_t idx, size_t cols) {
    const FileDiff *f = vec_at(&v->patch->files, idx);
    buf_puts(o, idx == v->pick ? t->sel : t->ctx_text);
    char pre[32], post[64];
    snprintf(pre, sizeof pre, " %c %c  ", idx == v->file_idx ? '*' : ' ', diffstatus_letter(f->status));
    snprintf(post, sizeof post, "  +%zu -%zu ", f->adds, f->dels);
    path_line(o, pre, filediff_path(f), post, cols);
    buf_puts(o, t->reset);
}

void render_frame(Buf *out, const RenderView *v) {
    if (v->rows <= 0 || v->cols <= 0) return;
    const Theme *t = v->mono ? &THEME_MONO : v->truecolor ? &THEME_TRUE : &THEME_256;
    size_t cols = (size_t)v->cols;
    bool picker = v->picker && v->patch != NULL;

    move_to(out, 1);
    draw_header(out, t, v, cols);

    size_t h = render_body_rows(v->rows);
    for (size_t i = 0; i < h; i++) {
        move_to(out, i + 2);
        if (v->help) {
            draw_help_row(out, t, i, cols);
            continue;
        }
        size_t idx;
        if (picker) {
            if (checked_add(v->pick_top, i, &idx) && idx < v->patch->files.len) draw_pick_row(out, t, v, idx, cols);
            else buf_puts(out, "\x1b[0m\x1b[2K");
            continue;
        }
        if (checked_add(v->top, i, &idx) && idx < v->model->rows.len) {
            const Row *r = vec_at(&v->model->rows, idx);
            if (r->kind == ROW_HUNK || r->kind == ROW_NOTE) draw_text_row(out, t, v, r, cols);
            else draw_diff_row(out, t, v, r, cols);
        } else {
            buf_puts(out, "\x1b[0m\x1b[2K");
        }
    }

    if (v->rows >= 2) {
        move_to(out, (size_t)v->rows);
        draw_bar(out, t, v, cols);
    }
}
