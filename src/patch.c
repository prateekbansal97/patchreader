#include "patch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- errors ---- */

static void set_err(ParseError *e, size_t line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(ParseError *e, size_t line, const char *fmt, ...) {
    if (!e) return;
    e->line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

/* ---- line slices and cursor ---- */

typedef struct {
    const char *p;
    size_t n;   /* length without the line ending */
    size_t off; /* offset of p in the source */
} Sl;

#define SKIPLIT(s, lit) sl_skip((s), sizeof(lit) - 1)

static bool sl_starts(Sl s, const char *pre) {
    size_t n = strlen(pre);
    return s.n >= n && memcmp(s.p, pre, n) == 0;
}

static Sl sl_skip(Sl s, size_t k) { /* caller guarantees k <= s.n */
    s.p += k;
    s.n -= k;
    s.off += k;
    return s;
}

/* Line starting at pos; strips "\n" and a preceding "\r". */
static bool line_at(const char *src, size_t len, size_t pos, Sl *out, size_t *next) {
    if (pos >= len) return false;
    const char *nl = memchr(src + pos, '\n', len - pos);
    size_t end = nl ? (size_t)(nl - src) : len;
    size_t n = end - pos;
    if (n > 0 && src[end - 1] == '\r') n--;
    out->p = src + pos;
    out->n = n;
    out->off = pos;
    *next = nl ? end + 1 : len;
    return true;
}

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    size_t next;
    size_t lineno; /* 1-based number of the current line */
    Sl line;
    bool ok; /* false at end of input */
} Cur;

static void cur_load(Cur *c) {
    c->ok = line_at(c->src, c->len, c->pos, &c->line, &c->next);
}

static void cur_init(Cur *c, const char *src, size_t len) {
    c->src = src;
    c->len = len;
    c->pos = 0;
    c->next = 0;
    c->lineno = 1;
    cur_load(c);
}

static void cur_advance(Cur *c) {
    c->pos = c->next;
    c->lineno++;
    cur_load(c);
}

/* "--- x" at the cursor starts a file only when "+++ y" and a hunk header
 * follow; this keeps diffs quoted in commit messages from being taken for
 * files. The "+++" line is returned in *l2. */
static bool header_pair(const Cur *c, Sl *l2) {
    size_t after, unused;
    Sl l3;
    if (!line_at(c->src, c->len, c->next, l2, &after) || !sl_starts(*l2, "+++ ")) return false;
    return line_at(c->src, c->len, after, &l3, &unused) &&
           (sl_starts(l3, "@@ -") || sl_starts(l3, "@@@ "));
}

/* ---- paths ---- */

/* s starts with '"'. Decodes C-style escapes up to the closing quote.
 * Returns the bytes consumed including both quotes, or 0 if unterminated. */
static size_t unquote_c(Sl s, Buf *out) {
    size_t i = 1;
    while (i < s.n) {
        char ch = s.p[i];
        if (ch == '"') return i + 1;
        if (ch == '\\' && i + 1 < s.n) {
            i++;
            char e = s.p[i];
            if (e >= '0' && e <= '7') {
                unsigned v = 0;
                for (size_t k = 0; k < 3 && i < s.n && s.p[i] >= '0' && s.p[i] <= '7'; k++, i++)
                    v = v * 8u + (unsigned)(s.p[i] - '0');
                buf_putc(out, (char)(v & 0xffu));
                continue;
            }
            if (e == 'n') e = '\n';
            else if (e == 't') e = '\t';
            else if (e == 'r') e = '\r';
            buf_putc(out, e);
            i++;
            continue;
        }
        buf_putc(out, ch);
        i++;
    }
    return 0;
}

/* Turns a "--- x" / "+++ x" / "rename from x" operand into an arena string.
 * Returns NULL for /dev/null. A non-zero prefix ('a' or 'b') is stripped
 * together with its '/'. */
static const char *parse_name(Patch *p, Sl s, char prefix) {
    Buf tmp;
    buf_init(&tmp);
    size_t used = 0;
    if (s.n > 0 && s.p[0] == '"') used = unquote_c(s, &tmp);
    if (used == 0) {
        buf_clear(&tmp);
        const char *tab = memchr(s.p, '\t', s.n); /* drop a trailing timestamp */
        buf_append(&tmp, s.p, tab ? (size_t)(tab - s.p) : s.n);
    }
    const char *r = tmp.data;
    size_t rn = tmp.len;
    const char *res = NULL;
    if (!(rn == 9 && memcmp(r, "/dev/null", 9) == 0)) {
        if (prefix && rn >= 2 && r[0] == prefix && r[1] == '/') {
            r += 2;
            rn -= 2;
        }
        res = arena_strndup(&p->arena, r, rn);
    }
    buf_free(&tmp);
    return res;
}

static void apply_names(FileDiff *f, const char *o, const char *n) {
    f->saw_names = true;
    f->old_path = o;
    f->new_path = n;
    if (!o && n) f->status = DIFF_ADDED;
    else if (o && !n) f->status = DIFF_DELETED;
}

/* "diff --git a/x b/y": paths may contain spaces, so look for the split where
 * both halves name the same file; fall back to the first space. */
static void parse_git_paths(Patch *p, FileDiff *f, Sl rest) {
    Sl a = rest, b = rest;
    bool have = false;

    if (rest.n > 0 && rest.p[0] == '"') {
        Buf tmp;
        buf_init(&tmp);
        size_t used = unquote_c(rest, &tmp);
        buf_free(&tmp);
        if (used > 0 && used < rest.n && rest.p[used] == ' ') {
            a = (Sl){rest.p, used, rest.off};
            b = sl_skip(rest, used + 1);
            have = true;
        }
    } else {
        size_t first_space = rest.n;
        for (size_t i = 0; i < rest.n && !have; i++) {
            if (rest.p[i] != ' ') continue;
            if (first_space == rest.n) first_space = i;
            Sl x = {rest.p, i, rest.off};
            Sl y = sl_skip(rest, i + 1);
            Sl xs = x, ys = y;
            if (xs.n >= 2 && xs.p[0] == 'a' && xs.p[1] == '/') xs = sl_skip(xs, 2);
            if (ys.n >= 2 && ys.p[0] == 'b' && ys.p[1] == '/') ys = sl_skip(ys, 2);
            if (xs.n == ys.n && memcmp(xs.p, ys.p, xs.n) == 0) {
                a = x;
                b = y;
                have = true;
            }
        }
        if (!have && first_space < rest.n) {
            a = (Sl){rest.p, first_space, rest.off};
            b = sl_skip(rest, first_space + 1);
        }
    }
    f->old_path = parse_name(p, a, 'a');
    f->new_path = parse_name(p, b, 'b');
}

/* Extended git header lines between "diff --git" and the first hunk. */
static bool ext_header(Patch *p, FileDiff *f, Sl l, bool *skip_bin) {
    if (sl_starts(l, "new file mode ")) {
        f->status = DIFF_ADDED;
    } else if (sl_starts(l, "deleted file mode ")) {
        f->status = DIFF_DELETED;
    } else if (sl_starts(l, "rename from ")) {
        f->status = DIFF_RENAMED;
        f->old_path = parse_name(p, SKIPLIT(l, "rename from "), 0);
    } else if (sl_starts(l, "rename to ")) {
        f->status = DIFF_RENAMED;
        f->new_path = parse_name(p, SKIPLIT(l, "rename to "), 0);
    } else if (sl_starts(l, "copy from ")) {
        f->status = DIFF_COPIED;
        f->old_path = parse_name(p, SKIPLIT(l, "copy from "), 0);
    } else if (sl_starts(l, "copy to ")) {
        f->status = DIFF_COPIED;
        f->new_path = parse_name(p, SKIPLIT(l, "copy to "), 0);
    } else if (sl_starts(l, "Binary files ")) {
        f->is_binary = true;
    } else if (sl_starts(l, "GIT binary patch")) {
        f->is_binary = true;
        *skip_bin = true;
    } else if (!(sl_starts(l, "old mode ") || sl_starts(l, "new mode ") ||
                 sl_starts(l, "similarity index ") || sl_starts(l, "dissimilarity index ") ||
                 sl_starts(l, "index "))) {
        return false;
    }
    return true;
}

/* ---- file list ---- */

static FileDiff *new_file(Patch *p) {
    FileDiff *f = vec_push(&p->files);
    f->status = DIFF_MODIFIED;
    vec_init(&f->hunks, sizeof(Hunk));
    return f;
}

/* "Binary files A and B differ" from a plain (non-git) diff. */
static bool plain_binary(Patch *p, Sl l) {
    static const char tail[] = " differ";
    if (!sl_starts(l, "Binary files ") || l.n < sizeof tail - 1 ||
        memcmp(l.p + l.n - (sizeof tail - 1), tail, sizeof tail - 1) != 0)
        return false;
    Sl rest = SKIPLIT(l, "Binary files ");
    rest.n -= sizeof tail - 1;
    Sl a = rest, b = rest;
    for (size_t i = 0; i + 5 <= rest.n; i++) {
        if (memcmp(rest.p + i, " and ", 5) == 0) {
            a = (Sl){rest.p, i, rest.off};
            b = sl_skip(rest, i + 5);
            break;
        }
    }
    FileDiff *f = new_file(p);
    f->is_binary = true;
    apply_names(f, parse_name(p, a, 0), parse_name(p, b, 0));
    return true;
}

/* ---- hunks ---- */

static bool lit(const char **pp, const char *end, const char *s) {
    size_t n = strlen(s);
    if ((size_t)(end - *pp) < n || memcmp(*pp, s, n) != 0) return false;
    *pp += n;
    return true;
}

static bool num(const char **pp, const char *end, size_t *out) {
    const char *p = *pp;
    if (p >= end || *p < '0' || *p > '9') return false;
    size_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        size_t d = (size_t)(*p - '0');
        if (v > (PR_MAX_LINENO - d) / 10) return false;
        v = v * 10 + d;
        p++;
    }
    *out = v;
    *pp = p;
    return true;
}

/* "@@ -a[,b] +c[,d] @@[ heading]"; an omitted count means 1. */
static bool parse_hunk_header(const Cur *c, Hunk *h) {
    const char *p = c->line.p;
    const char *end = p + c->line.n;
    h->old_count = 1;
    h->new_count = 1;
    if (!lit(&p, end, "@@ -") || !num(&p, end, &h->old_start)) return false;
    if (p < end && *p == ',') {
        p++;
        if (!num(&p, end, &h->old_count)) return false;
    }
    if (!lit(&p, end, " +") || !num(&p, end, &h->new_start)) return false;
    if (p < end && *p == ',') {
        p++;
        if (!num(&p, end, &h->new_count)) return false;
    }
    if (!lit(&p, end, " @@")) return false;
    if (p < end && *p == ' ') p++;
    h->head_off = (size_t)(p - c->src);
    h->head_len = (size_t)(end - p);
    return true;
}

/* Consumes the hunk header and exactly as many body lines as the header
 * promises (plus any "\ No newline" markers). */
static int parse_hunk(FileDiff *f, Cur *c, ParseError *err) {
    Hunk hdr;
    memset(&hdr, 0, sizeof hdr);
    if (!parse_hunk_header(c, &hdr)) {
        set_err(err, c->lineno, "malformed hunk header");
        return -1;
    }
    size_t start_line = c->lineno;
    Hunk *h = vec_push(&f->hunks);
    *h = hdr;
    vec_init(&h->lines, sizeof(Line));
    size_t old_rem = h->old_count;
    size_t new_rem = h->new_count;
    cur_advance(c);

    while (c->ok) {
        Sl l = c->line;
        if (l.n > 0 && l.p[0] == '\\') {
            if (h->lines.len > 0)
                ((Line *)vec_at(&h->lines, h->lines.len - 1))->no_eol = true;
            cur_advance(c);
            continue;
        }
        if (old_rem == 0 && new_rem == 0) break;

        char t = l.n > 0 ? l.p[0] : ' '; /* tools may strip the lone space of an empty context line */
        LineKind kind = LINE_CONTEXT;
        bool ok = true;
        switch (t) {
        case ' ':
            ok = old_rem > 0 && new_rem > 0;
            if (ok) {
                old_rem--;
                new_rem--;
            }
            break;
        case '-':
            ok = old_rem > 0;
            if (ok) {
                old_rem--;
                kind = LINE_DEL;
                f->dels++;
            }
            break;
        case '+':
            ok = new_rem > 0;
            if (ok) {
                new_rem--;
                kind = LINE_ADD;
                f->adds++;
            }
            break;
        default:
            ok = false;
        }
        if (!ok) {
            set_err(err, c->lineno, "corrupt hunk (header at line %zu)", start_line);
            return -1;
        }
        Line *ln = vec_push(&h->lines);
        ln->kind = kind;
        ln->off = l.n > 0 ? l.off + 1 : l.off;
        ln->len = l.n > 0 ? l.n - 1 : 0;
        cur_advance(c);
    }
    if (old_rem > 0 || new_rem > 0) {
        set_err(err, c->lineno, "unexpected end of patch in hunk (header at line %zu)", start_line);
        return -1;
    }
    return 0;
}

/* ---- entry points ---- */

int patch_parse(Patch *p, const char *src, size_t len, ParseError *err) {
    memset(p, 0, sizeof *p);
    p->src = src;
    p->src_len = len;
    arena_init(&p->arena);
    vec_init(&p->files, sizeof(FileDiff));
    if (err) {
        err->line = 0;
        err->msg[0] = '\0';
    }

    size_t newlines = 0;
    for (const char *q = src, *end = src + len; (q = memchr(q, '\n', (size_t)(end - q))) != NULL; q++)
        newlines++;
    if (newlines > PR_MAX_LINES) {
        set_err(err, 0, "too many lines (the limit is %zu)", PR_MAX_LINES);
        return -1;
    }

    Cur c;
    cur_init(&c, src, len);
    FileDiff *cur = NULL;
    bool skip_bin = false;

    while (c.ok) {
        Sl l = c.line;

        if (sl_starts(l, "diff --git ")) {
            cur = new_file(p);
            cur->git_ = true;
            parse_git_paths(p, cur, SKIPLIT(l, "diff --git "));
            skip_bin = false;
            cur_advance(&c);
            continue;
        }
        if (skip_bin) {
            cur_advance(&c);
            continue;
        }

        bool in_git_header = cur && cur->git_ && !cur->saw_names && cur->hunks.len == 0;
        if (in_git_header && ext_header(p, cur, l, &skip_bin)) {
            cur_advance(&c);
            continue;
        }

        Sl l2;
        if (sl_starts(l, "--- ") && header_pair(&c, &l2)) {
            if (!in_git_header) cur = new_file(p);
            char po = cur->git_ ? 'a' : 0;
            char pn = cur->git_ ? 'b' : 0;
            apply_names(cur, parse_name(p, SKIPLIT(l, "--- "), po),
                        parse_name(p, SKIPLIT(l2, "+++ "), pn));
            cur_advance(&c);
            cur_advance(&c);
            continue;
        }

        if (!in_git_header && plain_binary(p, l)) {
            cur = vec_at(&p->files, p->files.len - 1);
            cur_advance(&c);
            continue;
        }

        if (cur && sl_starts(l, "@@@")) {
            set_err(err, c.lineno, "combined diffs are not supported");
            return -1;
        }
        if (cur && sl_starts(l, "@@ -")) {
            if (parse_hunk(cur, &c, err) != 0) return -1;
            continue;
        }

        cur_advance(&c); /* preamble, commit message, diffstat, signature, ... */
    }

    if (p->files.len == 0) {
        set_err(err, 0, "no diff found in input");
        return -1;
    }
    return 0;
}

void patch_free(Patch *p) {
    for (size_t i = 0; i < p->files.len; i++) {
        FileDiff *f = vec_at(&p->files, i);
        for (size_t j = 0; j < f->hunks.len; j++) {
            Hunk *h = vec_at(&f->hunks, j);
            vec_free(&h->lines);
        }
        vec_free(&f->hunks);
    }
    vec_free(&p->files);
    arena_free(&p->arena);
    memset(p, 0, sizeof *p);
}

const char *filediff_path(const FileDiff *f) {
    if (f->new_path) return f->new_path;
    if (f->old_path) return f->old_path;
    return "(unknown)";
}

char diffstatus_letter(DiffStatus s) {
    switch (s) {
    case DIFF_ADDED: return 'A';
    case DIFF_DELETED: return 'D';
    case DIFF_RENAMED: return 'R';
    case DIFF_COPIED: return 'C';
    case DIFF_MODIFIED: break;
    }
    return 'M';
}