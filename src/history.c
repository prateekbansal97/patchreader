#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

void history_init(History *h) {
    memset(h, 0, sizeof *h);
}

static bool entry_ok(const char *s, size_t len) {
    if (len == 0 || len >= HISTORY_TEXT) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool history_add(History *h, const char *s, size_t len) {
    if (!entry_ok(s, len)) return false;
    size_t drop = h->count == HISTORY_MAX ? 0 : SIZE_MAX;
    for (size_t i = 0; i < h->count; i++) {
        if (strlen(h->items[i]) == len && memcmp(h->items[i], s, len) == 0) {
            drop = i;
            break;
        }
    }
    if (drop != SIZE_MAX) {
        memmove(h->items[drop], h->items[drop + 1], (h->count - drop - 1) * sizeof h->items[0]);
        h->count--;
    }
    memcpy(h->items[h->count], s, len);
    h->items[h->count][len] = '\0';
    h->count++;
    return true;
}

bool history_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/') return false;
    int k = snprintf(out, cap, "%s/.patchreader_history", home);
    return k > 0 && (size_t)k < cap;
}

void history_load(History *h, const char *path) {
    Buf b;
    buf_init(&b);
    if (read_path(path, &b, HISTORY_FILE_MAX) == READ_OK) {
        const char *p = b.data;
        const char *end = b.data + b.len;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            history_add(h, p, len);
            p += len + (nl ? 1 : 0);
        }
    }
    buf_free(&b);
}

bool history_save(const History *h, const char *path) {
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < h->count; i++) {
        buf_puts(&b, h->items[i]);
        buf_putc(&b, '\n');
    }

    char tmp[1100];
    int k = snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    bool ok = k > 0 && (size_t)k < sizeof tmp;
    if (ok) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        ok = fd >= 0;
        if (ok) {
            size_t done = 0;
            while (ok && done < b.len) {
                ssize_t w = write(fd, b.data + done, b.len - done);
                if (w >= 0) done += (size_t)w;
                else if (errno != EINTR) ok = false;
            }
            if (close(fd) != 0) ok = false;
            if (ok && rename(tmp, path) != 0) ok = false;
            if (!ok) (void)unlink(tmp);
        }
    }
    buf_free(&b);
    return ok;
}
