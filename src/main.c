#include <errno.h>
#include <stdio.h>
#include <string.h>

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

    size_t lines = 0;
    for (size_t k = 0; k < buf.len; k++)
        if (buf.data[k] == '\n') lines++;
    if (buf.len > 0 && buf.data[buf.len - 1] != '\n') lines++;
    printf("read %zu bytes, %zu lines\n", buf.len, lines);

    buf_free(&buf);
    return 0;
}