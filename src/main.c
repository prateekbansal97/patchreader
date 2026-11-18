#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "patch.h"
#include "util.h"

#define PR_VERSION "0.1.0"

static void usage(FILE *out) {
    fputs("usage: patchreader [options] <patch-file | ->\n"
          "\n"
          "Read-only terminal viewer for .patch / .diff files.\n"
          "\n"
          "options:\n"
          "  -h, --help     show this help\n"
          "  -V, --version  show version\n",
          out);
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
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
    for (size_t k = 0; k < patch.files.len; k++) {
        const FileDiff *f = vec_at(&patch.files, k);
        printf("%c %s  +%zu -%zu  %zu hunk(s)%s\n", diffstatus_letter(f->status),
               filediff_path(f), f->adds, f->dels, f->hunks.len,
               f->is_binary ? "  [binary]" : "");
    }
    patch_free(&patch);

    buf_free(&buf);
    return 0;
}