#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "model.h"
#include "input.h"
#include "patch.h"
#include "render.h"
#include "term.h"
#include "text.h"
#include "util.h"

#define PR_VERSION "0.1.0"

static void usage(FILE *out) {
    fputs("usage: patchreader [options] <patch-file | ->\n"
          "\n"
          "Read-only terminal viewer for .patch / .diff files.\n"
          "\n"
          "options:\n"
          "  --list         print one summary line per file and exit\n"
          "  --dump         print every file's rows as plain text and exit\n"
          "  -h, --help     show this help\n"
          "  -V, --version  show version\n",
          out);
}

static void dump_file(const Patch *patch, const FileDiff *f) {
    Model m;
    model_build(&m, patch, f);
    printf("=== %c %s\n", diffstatus_letter(f->status), filediff_path(f));
    Buf line;
    buf_init(&line);
    for (size_t k = 0; k < m.rows.len; k++) {
        const Row *r = vec_at(&m.rows, k);
        buf_clear(&line);
        text_window(&line, r->text, r->len, 0, SIZE_MAX);
        if (r->kind == ROW_HUNK || r->kind == ROW_NOTE) {
            printf("%s\n", line.data);
            continue;
        }
        char sign = r->kind == ROW_ADD ? '+' : r->kind == ROW_DEL ? '-' : ' ';
        if (r->old_no) printf("%*zu ", (int)m.old_w, r->old_no);
        else printf("%*s ", (int)m.old_w, "");
        if (r->new_no) printf("%*zu ", (int)m.new_w, r->new_no);
        else printf("%*s ", (int)m.new_w, "");
        printf("%c %s%s\n", sign, line.data, r->no_eol ? "  [no newline]" : "");
    }
    buf_free(&line);
    model_free(&m);
}

static bool want_truecolor(void) {
    const char *c = getenv("COLORTERM");
    return c && (strstr(c, "truecolor") || strstr(c, "24bit"));
}

static size_t scroll_down(size_t top, size_t n, size_t max_top) {
    return n > max_top - top ? max_top : top + n;
}

static size_t scroll_up(size_t top, size_t n) {
    return top > n ? top - n : 0;
}

static int run_ui(const Patch *patch) {
    Model m;
    model_build(&m, patch, vec_at(&patch->files, 0));
    if (!term_init()) {
        fprintf(stderr, "patchreader: cannot open the terminal\n");
        model_free(&m);
        return 1;
    }

    RenderView v;
    memset(&v, 0, sizeof v);
    v.model = &m;
    v.file_count = patch->files.len;
    v.truecolor = want_truecolor();

    Buf frame;
    buf_init(&frame);
    bool clear = true;
    bool quit = false;
    while (!quit) {
        term_size(&v.rows, &v.cols);
        size_t max_top = render_max_top(&m, v.rows);
        if (v.top > max_top) v.top = max_top;

        buf_clear(&frame);
        if (clear) {
            buf_puts(&frame, "\x1b[2J");
            clear = false;
        }
        render_frame(&frame, &v);
        term_write(frame.data, frame.len);

        size_t body = render_body_rows(v.rows);
        size_t page = body > 1 ? body - 1 : 1;
        size_t half = body > 1 ? body / 2 : 1;

        Key k = term_read_key();
        switch (k.type) {
        case KEY_EOF: quit = true; break;
        case KEY_RESIZE: clear = true; break;
        case KEY_DOWN:
        case KEY_ENTER: v.top = scroll_down(v.top, 1, max_top); break;
        case KEY_UP: v.top = scroll_up(v.top, 1); break;
        case KEY_PGDN: v.top = scroll_down(v.top, page, max_top); break;
        case KEY_PGUP: v.top = scroll_up(v.top, page); break;
        case KEY_HOME: v.top = 0; break;
        case KEY_END: v.top = max_top; break;
        case KEY_CTRL:
            if (k.ch == 'd') v.top = scroll_down(v.top, half, max_top);
            else if (k.ch == 'u') v.top = scroll_up(v.top, half);
            break;
        case KEY_CHAR:
            switch (k.ch) {
            case 'q': quit = true; break;
            case 'j': v.top = scroll_down(v.top, 1, max_top); break;
            case 'k': v.top = scroll_up(v.top, 1); break;
            case ' ': v.top = scroll_down(v.top, page, max_top); break;
            case 'b': v.top = scroll_up(v.top, page); break;
            case 'g': v.top = 0; break;
            case 'G': v.top = max_top; break;
            default: break;
            }
            break;
        default: break;
        }
    }
    term_restore();
    buf_free(&frame);
    model_free(&m);
    return 0;
}

int main(int argc, char **argv) {
    bool list = false;
    bool dump = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--list") == 0) {
            list = true;
            continue;
        }
        if (strcmp(a, "--dump") == 0) {
            dump = true;
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            puts("patchreader " PR_VERSION);
            return 0;
        }
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "patchreader: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        }
        break;
    }
    if (i != argc - 1) {
        usage(stderr);
        return 2;
    }
    const char *path = argv[i];

    Buf buf;
    buf_init(&buf);
    ReadStatus st = strcmp(path, "-") == 0 ? read_stream(stdin, &buf, PR_MAX_INPUT)
                                           : read_path(path, &buf, PR_MAX_INPUT);
    if (st == READ_IO) {
        fprintf(stderr, "patchreader: cannot read '%s': %s\n", path, strerror(errno));
        buf_free(&buf);
        return 1;
    }
    if (st == READ_TOO_BIG) {
        fprintf(stderr, "patchreader: '%s' is too large\n", path);
        buf_free(&buf);
        return 1;
    }

    Patch patch;
    ParseError perr;
    if (patch_parse(&patch, buf.data, buf.len, &perr) != 0) {
        if (perr.line)
            fprintf(stderr, "patchreader: %s:%zu: %s\n", path, perr.line, perr.msg);
        else
            fprintf(stderr, "patchreader: %s: %s\n", path, perr.msg);
        patch_free(&patch);
        buf_free(&buf);
        return 1;
    }
    if (!dump && !list && isatty(STDOUT_FILENO)) {
        int rc = run_ui(&patch);
        patch_free(&patch);
        buf_free(&buf);
        return rc;
    }
    for (size_t k = 0; k < patch.files.len; k++) {
        const FileDiff *f = vec_at(&patch.files, k);
        if (dump) {
            dump_file(&patch, f);
            continue;
        }
        printf("%c %s  +%zu -%zu  %zu hunk(s)%s\n", diffstatus_letter(f->status),
               filediff_path(f), f->adds, f->dels, f->hunks.len,
               f->is_binary ? "  [binary]" : "");
    }
    patch_free(&patch);

    buf_free(&buf);
    return 0;
}