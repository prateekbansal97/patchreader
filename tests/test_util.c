#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "util.h"

#define MIB ((size_t)1 << 20)

/* ---- death tests: the child must exit with status 1 via die() ---- */

static int dies_with_exit1(void (*fn)(void)) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        if (!freopen("/dev/null", "w", stderr)) _exit(3);
        fn();
        _exit(0); /* fn returned: it did not die */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        exit(2);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 1;
}

/* Each helper dies before allocating anything, so LeakSanitizer stays quiet. */
static void d_vec_oob(void) {
    Vec v;
    vec_init(&v, sizeof(int));
    (void)vec_at(&v, 0);
}
static void d_vec_zero_elem(void) {
    Vec v;
    vec_init(&v, 0);
}
static void d_vec_huge_elem(void) {
    Vec v;
    vec_init(&v, SIZE_MAX / 2 + 1);
    (void)vec_push(&v);
}
static void d_buf_huge(void) {
    Buf b = {0};
    buf_reserve(&b, SIZE_MAX);
}
static void d_arena_huge(void) {
    Arena a;
    arena_init(&a);
    (void)arena_alloc(&a, SIZE_MAX);
}
static void d_arena_strndup_huge(void) {
    Arena a;
    arena_init(&a);
    (void)arena_strndup(&a, "", SIZE_MAX);
}

static void test_deaths(void) {
    CHECK(dies_with_exit1(d_vec_oob));
    CHECK(dies_with_exit1(d_vec_zero_elem));
    CHECK(dies_with_exit1(d_vec_huge_elem));
    CHECK(dies_with_exit1(d_buf_huge));
    CHECK(dies_with_exit1(d_arena_huge));
    CHECK(dies_with_exit1(d_arena_strndup_huge));
}

/* ---- overflow helpers ---- */

static void test_checked(void) {
    size_t out = 0;
    CHECK(checked_add(1, 2, &out) && out == 3);
    CHECK(checked_add(SIZE_MAX, 0, &out) && out == SIZE_MAX);
    CHECK(!checked_add(SIZE_MAX, 1, &out));
    CHECK(!checked_add(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1, &out));
    CHECK(checked_mul(0, SIZE_MAX, &out) && out == 0);
    CHECK(checked_mul(MIB, MIB, &out) && out == MIB * MIB);
    CHECK(!checked_mul(SIZE_MAX / 2 + 1, 2, &out));
    CHECK(!checked_mul(SIZE_MAX, SIZE_MAX, &out));
}

/* ---- x* allocators ---- */

static void test_alloc(void) {
    void *p = xmalloc(0);
    CHECK(p != NULL);
    free(p);

    p = xcalloc(0, 0);
    CHECK(p != NULL);
    free(p);

    unsigned char *z = xcalloc(100, 1);
    int all_zero = 1;
    for (size_t i = 0; i < 100; i++)
        if (z[i] != 0) all_zero = 0;
    CHECK(all_zero);
    free(z);

    char *s = xrealloc(NULL, 4);
    memcpy(s, "abc", 4);
    s = xrealloc(s, 100000);
    CHECK(strcmp(s, "abc") == 0);
    free(s);
}

/* ---- Buf ---- */

static void test_buf(void) {
    Buf b;
    buf_init(&b);
    CHECK(b.data != NULL && b.len == 0 && b.cap >= 1 && b.data[0] == '\0');

    buf_puts(&b, "hello");
    buf_putc(&b, ' ');
    buf_append(&b, "world!!!", 5);
    CHECK(b.len == 11);
    CHECK(strcmp(b.data, "hello world") == 0);

    buf_append(&b, NULL, 0); /* zero-length append is a no-op */
    CHECK(b.len == 11 && b.data[11] == '\0');

    buf_clear(&b);
    CHECK(b.len == 0 && b.data[0] == '\0');

    /* growth across many reallocations, NUL kept after every append */
    for (size_t i = 0; i < 100000; i++) buf_putc(&b, (char)('a' + (i % 26)));
    CHECK(b.len == 100000);
    CHECK(b.cap > b.len);
    CHECK(b.data[b.len] == '\0');
    int intact = 1;
    for (size_t i = 0; i < b.len; i++)
        if (b.data[i] != (char)('a' + (i % 26))) intact = 0;
    CHECK(intact);

    /* one large append */
    char *big = xmalloc(MIB);
    memset(big, 'x', MIB);
    buf_clear(&b);
    buf_append(&b, big, MIB);
    CHECK(b.len == MIB && b.data[MIB] == '\0' && b.data[MIB - 1] == 'x');
    free(big);

    /* free resets, and a zeroed Buf is usable again */
    buf_free(&b);
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0);
    buf_putc(&b, 'z');
    CHECK(b.len == 1 && b.data[0] == 'z' && b.data[1] == '\0');

    /* embedded NUL bytes are kept */
    buf_append(&b, "a\0b", 3);
    CHECK(b.len == 4 && memcmp(b.data, "za\0b", 5) == 0);

    buf_free(&b);
}

/* ---- Vec ---- */

typedef struct {
    char tag;
    double d;
    uint64_t n;
} Rec;

static void test_vec(void) {
    Vec v;
    vec_init(&v, sizeof(int));
    CHECK(v.len == 0 && v.elem == sizeof(int));

    int fresh_zero = 1;
    for (int i = 0; i < 1000; i++) {
        int *p = vec_push(&v);
        if (*p != 0) fresh_zero = 0;
        *p = i * 3;
    }
    CHECK(fresh_zero);
    CHECK(v.len == 1000 && v.cap >= 1000);
    int intact = 1;
    for (size_t i = 0; i < 1000; i++)
        if (*(int *)vec_at(&v, i) != (int)i * 3) intact = 0;
    CHECK(intact);
    vec_free(&v);
    CHECK(v.data == NULL && v.len == 0 && v.cap == 0);

    /* struct elements: alignment and contents survive reallocation */
    vec_init(&v, sizeof(Rec));
    for (size_t i = 0; i < 500; i++) {
        Rec *r = vec_push(&v);
        r->tag = (char)('A' + (i % 26));
        r->d = (double)i;
        r->n = (uint64_t)i * 1000003u;
    }
    int ok = 1;
    for (size_t i = 0; i < 500; i++) {
        Rec *r = vec_at(&v, i);
        if ((uintptr_t)r % _Alignof(Rec) != 0) ok = 0;
        if (r->tag != (char)('A' + (i % 26)) || r->d != (double)i ||
            r->n != (uint64_t)i * 1000003u)
            ok = 0;
    }
    CHECK(ok);
    vec_free(&v);

    /* free on a never-pushed vec is fine */
    vec_init(&v, 8);
    vec_free(&v);
    CHECK(v.data == NULL);
}

/* ---- Arena ---- */

#define N_ALLOCS 2000

static void test_arena(void) {
    Arena a;
    arena_init(&a);
    CHECK(a.head == NULL);

    unsigned char *ptrs[N_ALLOCS];
    size_t sizes[N_ALLOCS];
    int aligned = 1, zeroed = 1;
    for (size_t i = 0; i < N_ALLOCS; i++) {
        sizes[i] = (i * 37) % 300;
        ptrs[i] = arena_alloc(&a, sizes[i]);
        if (ptrs[i] == NULL) {
            CHECK(ptrs[i] != NULL);
            return;
        }
        if ((uintptr_t)ptrs[i] % 16 != 0) aligned = 0;
        for (size_t k = 0; k < sizes[i]; k++)
            if (ptrs[i][k] != 0) zeroed = 0;
        memset(ptrs[i], (int)(i & 0xff), sizes[i]);
    }
    CHECK(aligned);
    CHECK(zeroed);

    /* earlier allocations must not have been overwritten by later ones */
    int intact = 1;
    for (size_t i = 0; i < N_ALLOCS; i++)
        for (size_t k = 0; k < sizes[i]; k++)
            if (ptrs[i][k] != (unsigned char)(i & 0xff)) intact = 0;
    CHECK(intact);

    /* an allocation larger than one block */
    size_t big_n = 200 * 1024;
    unsigned char *big = arena_alloc(&a, big_n);
    CHECK(big != NULL && (uintptr_t)big % 16 == 0);
    memset(big, 0xAB, big_n);
    CHECK(big[0] == 0xAB && big[big_n - 1] == 0xAB);

    /* zero-size allocations return distinct non-NULL pointers */
    void *z1 = arena_alloc(&a, 0);
    void *z2 = arena_alloc(&a, 0);
    CHECK(z1 != NULL && z2 != NULL && z1 != z2);

    /* strndup */
    char *s = arena_strndup(&a, "abcdef", 3);
    CHECK(strcmp(s, "abc") == 0);
    s = arena_strndup(&a, "xyz", 0);
    CHECK(s[0] == '\0');
    s = arena_strndup(&a, "a\0b", 3);
    CHECK(memcmp(s, "a\0b\0", 4) == 0);

    arena_free(&a);
    CHECK(a.head == NULL);

    /* usable again after free */
    char *again = arena_strndup(&a, "hi", 2);
    CHECK(strcmp(again, "hi") == 0);
    arena_free(&a);
}

/* ---- reading ---- */

static void test_read_stream(void) {
    FILE *fp = tmpfile();
    if (!fp) {
        CHECK(fp != NULL);
        return;
    }
    fputs("line1\nline2\n", fp);
    rewind(fp);
    Buf b;
    buf_init(&b);
    CHECK(read_stream(fp, &b, PR_MAX_INPUT) == READ_OK);
    CHECK(b.len == 12 && strcmp(b.data, "line1\nline2\n") == 0);
    fclose(fp);
    buf_free(&b);

    /* empty input */
    fp = tmpfile();
    if (!fp) {
        CHECK(fp != NULL);
        return;
    }
    buf_init(&b);
    CHECK(read_stream(fp, &b, PR_MAX_INPUT) == READ_OK && b.len == 0);
    fclose(fp);
    buf_free(&b);

    /* larger than the internal 64 KiB chunk */
    fp = tmpfile();
    if (!fp) {
        CHECK(fp != NULL);
        return;
    }
    for (size_t i = 0; i < 200000; i++) fputc((int)('a' + (i % 26)), fp);
    rewind(fp);
    buf_init(&b);
    CHECK(read_stream(fp, &b, PR_MAX_INPUT) == READ_OK);
    CHECK(b.len == 200000 && b.data[199999] == (char)('a' + (199999 % 26)));
    fclose(fp);
    buf_free(&b);

    /* size limit: exactly max is fine, one more is too big */
    fp = tmpfile();
    if (!fp) {
        CHECK(fp != NULL);
        return;
    }
    fputs("0123456789", fp);
    rewind(fp);
    buf_init(&b);
    CHECK(read_stream(fp, &b, 10) == READ_OK && b.len == 10);
    buf_free(&b);
    rewind(fp);
    buf_init(&b);
    CHECK(read_stream(fp, &b, 9) == READ_TOO_BIG);
    fclose(fp);
    buf_free(&b);

    /* I/O error: reading from a write-only stream */
    fp = fopen("/dev/null", "w");
    if (!fp) {
        CHECK(fp != NULL);
        return;
    }
    buf_init(&b);
    CHECK(read_stream(fp, &b, PR_MAX_INPUT) == READ_IO);
    fclose(fp);
    buf_free(&b);
}

static void test_read_path(void) {
    char tmpl[] = "/tmp/pr_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        CHECK(fd >= 0);
        return;
    }
    const char content[] = "diff --git a/x b/x\n+hello\n";
    size_t clen = sizeof content - 1;
    if (write(fd, content, clen) != (ssize_t)clen) {
        close(fd);
        unlink(tmpl);
        CHECK(0 && "write to temp file failed");
        return;
    }
    close(fd);

    Buf b;
    buf_init(&b);
    CHECK(read_path(tmpl, &b, PR_MAX_INPUT) == READ_OK);
    CHECK(b.len == clen && memcmp(b.data, content, clen) == 0);
    buf_free(&b);

    buf_init(&b);
    CHECK(read_path(tmpl, &b, 5) == READ_TOO_BIG);
    buf_free(&b);

    unlink(tmpl);

    buf_init(&b);
    errno = 0;
    CHECK(read_path(tmpl, &b, PR_MAX_INPUT) == READ_IO && errno == ENOENT);
    errno = 0;
    CHECK(read_path("", &b, PR_MAX_INPUT) == READ_IO && errno == ENOENT);
    errno = 0;
    CHECK(read_path("/", &b, PR_MAX_INPUT) == READ_IO && errno == EISDIR);
    CHECK(b.len == 0);
    buf_free(&b);
}

int main(void) {
    printf("test_util\n");
    test_deaths(); /* first: no live allocations when we fork */
    test_checked();
    test_alloc();
    test_buf();
    test_vec();
    test_arena();
    test_read_stream();
    test_read_path();
    TEST_DONE();
}
