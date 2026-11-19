#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "model.h"
#include "input.h"
#include "patch.h"
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

/* One full-width line at a screen row (1-based), clipped to the terminal. */
static void put_line(Buf *f, int row, int cols, const char *s, bool reverse) {
    char pos[32];
    int k = snprintf(pos, sizeof pos, "\x1b[%d;1H", row);
    if (k > 0) buf_append(f, pos, (size_t)k);
    if (reverse) buf_puts(f, "\x1b[7m");
    size_t shown = text_window(f, s, strlen(s), 0, (size_t)cols);
    if (reverse) {
        for (size_t i = shown; i < (size_t)cols; i++) buf_putc(f, ' ');
        buf_puts(f, "\x1b[0m");
    } else {
        buf_puts(f, "\x1b[K");
    }
}

/* Interim full-screen loop: proves the terminal layer works. */
static int run_ui(const Patch *patch, const char *path) {
    if (!term_init()) {
        fprintf(stderr, "patchreader: cannot open the terminal\n");
        return 1;
    }
    Buf frame;
    buf_init(&frame);
    char last[64] = "(none)";
    for (;;) {
        int rows, cols;
        term_size(&rows, &cols);
        char line[256];

        buf_clear(&frame);
        buf_puts(&frame, "\x1b[2J");
        snprintf(line, sizeof line, " patchreader  %s", path);
        put_line(&frame, 1, cols, line, true);
        snprintf(line, sizeof line, " %zu file(s), terminal %dx%d", patch->files.len, cols, rows);
        put_line(&frame, 3, cols, line, false);
        snprintf(line, sizeof line, " last key: %s", last);
        put_line(&frame, 5, cols, line, false);
        put_line(&frame, rows, cols, " q quit   (read-only)", true);
        term_write(frame.data, frame.len);

        Key k = term_read_key();
        if (k.type == KEY_EOF || (k.type == KEY_CHAR && k.ch == 'q')) break;
        key_describe(k, last, sizeof last);
    }
    term_restore();
    buf_free(&frame);
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
        int rc = run_ui(&patch, path);
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