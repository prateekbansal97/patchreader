#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void (*pr_die_hook)(void) = NULL;

void die(const char *fmt, ...) {
    va_list ap;
    if (pr_die_hook) {
        void (*hook)(void) = pr_die_hook;
        pr_die_hook = NULL;
        hook();
    }
    fputs("patchreader: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t size) {
    if (n == 0 || size == 0) {
        n = 1;
        size = 1;
    }
    void *p = calloc(n, size);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

/* ---- Buf ---- */

void buf_init(Buf *b) {
    b->cap = 64;
    b->data = xmalloc(b->cap);
    b->len = 0;
    b->data[0] = '\0';
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_clear(Buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void buf_reserve(Buf *b, size_t extra) {
    size_t need;
    if (!checked_add(b->len, extra, &need) || !checked_add(need, 1, &need))
        die("buffer too large");
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *src, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void buf_putc(Buf *b, char c) {
    buf_append(b, &c, 1);
}

/* ---- Vec ---- */

void vec_init(Vec *v, size_t elem) {
    if (elem == 0) die("vec element size must be non-zero");
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
}

void vec_free(Vec *v) {
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

void *vec_push(Vec *v) {
    if (v->len == v->cap) {
        size_t cap = 8;
        if (v->cap) {
            if (!checked_mul(v->cap, 2, &cap)) die("vector too large");
        }
        size_t bytes;
        if (!checked_mul(cap, v->elem, &bytes)) die("vector too large");
        v->data = xrealloc(v->data, bytes);
        v->cap = cap;
    }
    char *slot = (char *)v->data + v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void *vec_at(const Vec *v, size_t i) {
    if (i >= v->len) die("vec index %zu out of range (len %zu)", i, v->len);
    return (char *)v->data + i * v->elem;
}

/* ---- Arena ---- */

#define ARENA_BLOCK_SIZE ((size_t)64 * 1024)

/* Header is 4 words so that the data following it stays 16-byte aligned
 * (malloc returns 16-byte aligned memory on our targets). */
struct ArenaBlock {
    ArenaBlock *next;
    size_t used;
    size_t cap;
    size_t pad_;
};

void arena_init(Arena *a) {
    a->head = NULL;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t n) {
    size_t rounded;
    if (!checked_add(n, 15, &rounded)) die("arena allocation too large");
    rounded &= ~(size_t)15;
    if (rounded == 0) rounded = 16;

    ArenaBlock *b = a->head;
    if (b == NULL || b->cap - b->used < rounded) {
        size_t cap = ARENA_BLOCK_SIZE;
        if (rounded > cap) cap = rounded;
        size_t total;
        if (!checked_add(sizeof *b, cap, &total)) die("arena allocation too large");
        b = xmalloc(total);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        b->pad_ = 0;
        a->head = b;
    }
    void *p = (unsigned char *)(b + 1) + b->used;
    b->used += rounded;
    memset(p, 0, rounded);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    size_t total;
    if (!checked_add(n, 1, &total)) die("string too large");
    char *p = arena_alloc(a, total);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ---- Reading ---- */

ReadStatus read_stream(FILE *fp, Buf *out, size_t max) {
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) {
        if (out->len > max || n > max - out->len) return READ_TOO_BIG;
        buf_append(out, tmp, n);
    }
    if (ferror(fp)) return READ_IO;
    return READ_OK;
}

ReadStatus read_path(const char *path, Buf *out, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return READ_IO;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return READ_IO;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return READ_IO;
    }

    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        int e = errno;
        close(fd);
        errno = e;
        return READ_IO;
    }
    ReadStatus rs = read_stream(fp, out, max);
    int e = errno;
    fclose(fp);
    errno = e;
    return rs;
}

