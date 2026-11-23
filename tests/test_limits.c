#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "patch.h"
#include "util.h"

/* "--- /dev/null\n+++ b/x\n@@ -0,0 +1,<n> @@\n" followed by n lines "+\n";
 * the result has n + 3 newlines. The block is exactly sized, so ASan sees
 * any overread. */
static char *adds_patch(size_t n, size_t *len) {
    char head[96];
    int h = snprintf(head, sizeof head, "--- /dev/null\n+++ b/x\n@@ -0,0 +1,%zu @@\n", n);
    size_t total = (size_t)h + n * 2;
    char *p = xmalloc(total);
    memcpy(p, head, (size_t)h);
    for (size_t i = 0; i < n; i++) {
        p[(size_t)h + i * 2] = '+';
        p[(size_t)h + i * 2 + 1] = '\n';
    }
    *len = total;
    return p;
}

static void test_line_limit(void) {
    CHECK(PR_MAX_LINES == 10000000);

    /* exactly at the limit: accepted */
    size_t len;
    char *src = adds_patch(PR_MAX_LINES - 3, &len);
    Patch p;
    ParseError err;
    CHECK(patch_parse(&p, src, len, &err) == 0);
    CHECK(p.files.len == 1);
    if (p.files.len == 1) {
        const FileDiff *f = vec_at(&p.files, 0);
        CHECK(f->adds == PR_MAX_LINES - 3);
    }
    patch_free(&p);
    free(src);

    /* one line more: refused, cleanly */
    src = adds_patch(PR_MAX_LINES - 2, &len);
    CHECK(patch_parse(&p, src, len, &err) == -1);
    CHECK(err.line == 0);
    CHECK(strstr(err.msg, "too many lines") != NULL);
    CHECK(strstr(err.msg, "10000000") != NULL);
    CHECK(p.files.len == 0);
    patch_free(&p);
    free(src);

    /* the same without an error out-parameter */
    src = adds_patch(PR_MAX_LINES, &len);
    CHECK(patch_parse(&p, src, len, NULL) == -1);
    patch_free(&p);
    free(src);

    /* the limit counts lines, not bytes: long lines are fine */
    size_t big = 20u << 20;
    char *line = xmalloc(big + 40);
    size_t k = (size_t)snprintf(line, 40, "--- a/f\n+++ b/f\n@@ -1 +1 @@\n-");
    memset(line + k, 'x', big);
    k += big;
    memcpy(line + k, "\n+y\n", 4);
    k += 4;
    CHECK(patch_parse(&p, line, k, &err) == 0);
    CHECK(p.files.len == 1);
    patch_free(&p);
    free(line);

    /* nothing but newlines past the limit: refused before any parsing */
    size_t nl = PR_MAX_LINES + 1;
    char *blank = xmalloc(nl);
    memset(blank, '\n', nl);
    CHECK(patch_parse(&p, blank, nl, &err) == -1);
    CHECK(strstr(err.msg, "too many lines") != NULL);
    patch_free(&p);
    free(blank);
}

int main(void) {
    printf("test_limits\n");
    test_line_limit();
    TEST_DONE();
}
