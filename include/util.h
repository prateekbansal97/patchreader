#ifndef PR_UTIL_H
#define PR_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define PR_MAX_INPUT ((size_t)1 << 30)

/* Called once by die() before it prints (the terminal layer uses it to leave
 * the alternate screen so the message stays visible). */
extern void (*pr_die_hook)(void);

/* Fatal error: prints "patchreader: <msg>" to stderr and exits with 1. */
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Allocation wrappers: never return NULL, abort on out-of-memory. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);

/* Overflow-checked arithmetic. Return true on success. */
static inline bool checked_add(size_t a, size_t b, size_t *out) {
    return !__builtin_add_overflow(a, b, out);
}
static inline bool checked_mul(size_t a, size_t b, size_t *out) {
    return !__builtin_mul_overflow(a, b, out);
}

/* Growable byte buffer. Invariant after buf_init: data != NULL,
 * cap >= len + 1 and data[len] == '\0'. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const void *src, size_t n); /* src must not alias b */
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);

/* Growable array of fixed-size elements. */
typedef struct {
    void *data;
    size_t len;
    size_t cap;
    size_t elem;
} Vec;

void vec_init(Vec *v, size_t elem);
void vec_free(Vec *v);
void *vec_push(Vec *v);                /* pointer to a new zeroed slot */
void *vec_at(const Vec *v, size_t i);  /* aborts when i >= len */

/* Bump allocator; everything is released at once by arena_free. */
typedef struct ArenaBlock ArenaBlock;
typedef struct {
    ArenaBlock *head;
} Arena;

void arena_init(Arena *a);
void arena_free(Arena *a);
void *arena_alloc(Arena *a, size_t n); /* zeroed, 16-byte aligned */
char *arena_strndup(Arena *a, const char *s, size_t n);

/* Whole-input reading. */
typedef enum { READ_OK = 0, READ_IO, READ_TOO_BIG } ReadStatus;

ReadStatus read_stream(FILE *fp, Buf *out, size_t max);
ReadStatus read_path(const char *path, Buf *out, size_t max);

#endif