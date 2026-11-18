#ifndef PR_CHECK_H
#define PR_CHECK_H

#include <stdio.h>

/* Minimal test harness: CHECK records failures and keeps going;
 * TEST_DONE prints a summary and makes main() return non-zero on failure. */

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while (0)

#define TEST_DONE()                                                      \
    do {                                                                 \
        printf("  %d checks, %d failures\n", g_checks, g_failures);      \
        return g_failures ? 1 : 0;                                       \
    } while (0)

#endif
