#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "fuzz.h"
#include "history.h"
#include "util.h"

#define REQUIRE(cond)     \
    do {                  \
        int ok_ = (cond); \
        CHECK(ok_);       \
        if (!ok_) return; \
    } while (0)

/* ---- a private scratch directory for the files these tests write ---- */

static char g_dir[512];

static void dir_make(void) {
    const char *t = getenv("TMPDIR");
    if (t == NULL || t[0] == '\0') t = "/tmp";
    /* mkdtemp is not in strict POSIX 2008 on every system: name it by hand */
    for (int attempt = 0; attempt < 100; attempt++) {
        int k = snprintf(g_dir, sizeof g_dir, "%s/patchreader-hist-%ld-%d", t, (long)getpid(), attempt);
        if (k < 0 || (size_t)k >= sizeof g_dir) break;
        if (mkdir(g_dir, 0700) == 0) return;
        if (errno != EEXIST) break;
    }
    perror("test_history: cannot create a scratch directory");
    exit(2);
}

/* Full path of a file inside the scratch directory. */
static const char *P(const char *name) {
    static char ring[8][1200];
    static int next;
    char *p = ring[next++ % 8];
    int k = snprintf(p, sizeof ring[0], "%s/%s", g_dir, name);
    if (k < 0 || (size_t)k >= sizeof ring[0]) {
        fprintf(stderr, "test_history: path too long\n");
        exit(2);
    }
    return p;
}

static void dir_clean(void) {
    DIR *d = opendir(g_dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            const char *p = P(e->d_name);
            if (unlink(p) != 0) rmdir(p);
        }
        closedir(d);
    }
    rmdir(g_dir);
}

/* Number of directory entries whose name contains `part`. */
static int dir_count(const char *part) {
    int n = 0;
    DIR *d = opendir(g_dir);
    if (d == NULL) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strstr(e->d_name, part) != NULL) n++;
    }
    closedir(d);
    return n;
}

static void write_file(const char *name, const void *data, size_t n) {
    FILE *f = fopen(P(name), "wb");
    if (f == NULL) {
        perror("test_history: fopen");
        exit(2);
    }
    if (n > 0 && fwrite(data, 1, n, f) != n) {
        perror("test_history: fwrite");
        exit(2);
    }
    fclose(f);
}

static void write_str(const char *name, const char *s) {
    write_file(name, s, strlen(s));
}

/* Whole file into b; false if it cannot be read. */
static bool read_file(const char *name, Buf *b) {
    buf_clear(b);
    FILE *f = fopen(P(name), "rb");
    if (f == NULL) return false;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(b, tmp, n);
    fclose(f);
    return true;
}

static bool file_is(const char *name, const char *want) {
    Buf b;
    buf_init(&b);
    bool ok = read_file(name, &b) && b.len == strlen(want) && memcmp(b.data, want, b.len) == 0;
    buf_free(&b);
    return ok;
}

/* ---- helpers on History ---- */

/* The entry is copied to an exact-size heap block, so ASan flags any read
 * past len. */
static bool add_n(History *h, const void *s, size_t n) {
    char *p = xmalloc(n);
    if (n) memcpy(p, s, n);
    bool r = history_add(h, p, n);
    free(p);
    return r;
}

static bool add(History *h, const char *s) {
    return add_n(h, s, strlen(s));
}

static bool is(const History *h, size_t i, const char *s) {
    return i < h->count && strcmp(h->items[i], s) == 0;
}

/* Everything history_add promises about the list as a whole. */
static bool valid(const History *h) {
    if (h->count > HISTORY_MAX) return false;
    for (size_t i = 0; i < h->count; i++) {
        size_t len = strlen(h->items[i]);
        if (len == 0 || len >= HISTORY_TEXT) return false;
        for (size_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)h->items[i][k];
            if (c < 0x20 || c == 0x7F) return false;
        }
        for (size_t j = i + 1; j < h->count; j++)
            if (strcmp(h->items[i], h->items[j]) == 0) return false;
    }
    return true;
}

static bool same(const History *a, const History *b) {
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i], b->items[i]) != 0) return false;
    return true;
}

/* ---- history_add ---- */

static void test_add(void) {
    History h;
    history_init(&h);
    CHECK(h.count == 0);

    CHECK(add(&h, "one") && add(&h, "two") && add(&h, "three"));
    CHECK(h.count == 3 && is(&h, 0, "one") && is(&h, 1, "two") && is(&h, 2, "three"));

    /* an equal entry moves to the end instead of appearing twice */
    CHECK(add(&h, "one"));
    CHECK(h.count == 3 && is(&h, 0, "two") && is(&h, 1, "three") && is(&h, 2, "one"));
    CHECK(add(&h, "one")); /* already the newest */
    CHECK(h.count == 3 && is(&h, 0, "two") && is(&h, 1, "three") && is(&h, 2, "one"));
    CHECK(add(&h, "two")); /* the oldest */
    CHECK(h.count == 3 && is(&h, 0, "three") && is(&h, 1, "one") && is(&h, 2, "two"));

    /* a prefix or an extension is a different entry */
    CHECK(add(&h, "tw") && add(&h, "twoo"));
    CHECK(h.count == 5 && is(&h, 3, "tw") && is(&h, 4, "twoo"));

    /* case counts: the search itself is smart-case, the history is exact */
    CHECK(add(&h, "Two"));
    CHECK(h.count == 6 && is(&h, 5, "Two") && is(&h, 2, "two"));

    /* UTF-8 text and spaces are fine */
    CHECK(add(&h, "\xc3\xa9\xe6\x97\xa5 two words"));
    CHECK(h.count == 7 && valid(&h));
}

static void test_add_rejects(void) {
    History h;
    history_init(&h);
    REQUIRE(add(&h, "keep"));

    CHECK(!add_n(&h, "", 0));
    CHECK(!add(&h, "a\nb"));
    CHECK(!add(&h, "a\tb"));
    CHECK(!add(&h, "a\rb"));
    CHECK(!add(&h, "a\x7f"));
    CHECK(!add(&h, "\x1b[31mred"));
    CHECK(!add_n(&h, "a\0b", 3)); /* an embedded NUL is a control byte */
    CHECK(!add_n(&h, "\0", 1));

    char big[200];
    memset(big, 'x', sizeof big);
    CHECK(!add_n(&h, big, HISTORY_TEXT));     /* does not fit with its NUL */
    CHECK(!add_n(&h, big, HISTORY_TEXT + 50));
    CHECK(!add_n(&h, big, sizeof big));
    CHECK(h.count == 1 && is(&h, 0, "keep")); /* nothing changed */

    CHECK(add_n(&h, big, HISTORY_TEXT - 1)); /* the longest entry */
    CHECK(h.count == 2 && strlen(h.items[1]) == HISTORY_TEXT - 1);
    CHECK(h.items[1][HISTORY_TEXT - 1] == '\0');
    CHECK(valid(&h));
}

static void test_add_full(void) {
    History h;
    history_init(&h);
    char s[32];
    for (int i = 0; i < 150; i++) {
        snprintf(s, sizeof s, "entry-%d", i);
        CHECK(add(&h, s));
    }
    CHECK(h.count == HISTORY_MAX);
    CHECK(is(&h, 0, "entry-50") && is(&h, HISTORY_MAX - 1, "entry-149"));
    CHECK(valid(&h));

    /* a repeat of the oldest entry when full: nothing is lost but it moves */
    CHECK(add(&h, "entry-50"));
    CHECK(h.count == HISTORY_MAX);
    CHECK(is(&h, 0, "entry-51") && is(&h, HISTORY_MAX - 1, "entry-50"));

    /* a repeat from the middle */
    CHECK(add(&h, "entry-100"));
    CHECK(h.count == HISTORY_MAX);
    CHECK(is(&h, HISTORY_MAX - 1, "entry-100") && is(&h, 0, "entry-51"));
    CHECK(valid(&h));

    /* a new entry when full drops the oldest */
    CHECK(add(&h, "fresh"));
    CHECK(h.count == HISTORY_MAX);
    CHECK(is(&h, 0, "entry-52") && is(&h, HISTORY_MAX - 1, "fresh"));
    CHECK(valid(&h));
}

/* ---- history_path ---- */

static void test_path(void) {
    const char *old = getenv("HOME");
    char *saved = old ? strdup(old) : NULL;
    char out[64];

    setenv("HOME", "/home/u", 1);
    CHECK(history_path(out, sizeof out));
    CHECK(strcmp(out, "/home/u/.patchreader_history") == 0);
    size_t need = strlen("/home/u/.patchreader_history") + 1;
    CHECK(history_path(out, need));      /* exact fit */
    CHECK(!history_path(out, need - 1)); /* one byte short */
    CHECK(!history_path(out, 1));
    CHECK(!history_path(out, 0));
    CHECK(!history_path(NULL, 0));

    setenv("HOME", "/", 1);
    CHECK(history_path(out, sizeof out) && strcmp(out, "//.patchreader_history") == 0);

    setenv("HOME", "relative/dir", 1);
    CHECK(!history_path(out, sizeof out));
    setenv("HOME", "", 1);
    CHECK(!history_path(out, sizeof out));
    unsetenv("HOME");
    CHECK(!history_path(out, sizeof out));

    char longhome[600];
    longhome[0] = '/';
    memset(longhome + 1, 'h', sizeof longhome - 2);
    longhome[sizeof longhome - 1] = '\0';
    setenv("HOME", longhome, 1);
    CHECK(!history_path(out, sizeof out));
    char wide[1024];
    CHECK(history_path(wide, sizeof wide));
    CHECK(strlen(wide) == strlen(longhome) + strlen("/.patchreader_history"));

    if (saved) {
        setenv("HOME", saved, 1);
        free(saved);
    } else {
        unsetenv("HOME");
    }
}

/* ---- history_load ---- */

static void load(History *h, const char *content, size_t n) {
    write_file("hist", content, n);
    history_load(h, P("hist"));
}

static void load_str(History *h, const char *content) {
    load(h, content, strlen(content));
}

static void test_load(void) {
    History h;

    /* missing file: nothing added, nothing broken */
    history_init(&h);
    history_load(&h, P("does-not-exist"));
    CHECK(h.count == 0);
    CHECK(add(&h, "x"));
    history_load(&h, P("does-not-exist"));
    CHECK(h.count == 1 && is(&h, 0, "x"));

    history_init(&h);
    load_str(&h, "");
    CHECK(h.count == 0);

    history_init(&h);
    load_str(&h, "a\nb\nc\n");
    CHECK(h.count == 3 && is(&h, 0, "a") && is(&h, 1, "b") && is(&h, 2, "c"));

    /* no newline after the last line */
    history_init(&h);
    load_str(&h, "a\nb");
    CHECK(h.count == 2 && is(&h, 1, "b"));

    /* blank lines, CRLF, tabs and DEL are not valid entries */
    history_init(&h);
    load_str(&h, "a\n\n\r\nb\r\n\tc\nd\x7f\ne\n\n\n");
    CHECK(h.count == 2 && is(&h, 0, "a") && is(&h, 1, "e"));

    /* NUL bytes in the file */
    history_init(&h);
    load(&h, "a\n\0\nb\0c\nz\n", 10);
    CHECK(h.count == 2 && is(&h, 0, "a") && is(&h, 1, "z"));

    /* length limit: 95 bytes is kept, 96 is not */
    char line[128];
    memset(line, 'k', sizeof line);
    Buf b;
    buf_init(&b);
    buf_append(&b, line, HISTORY_TEXT - 1);
    buf_putc(&b, '\n');
    buf_append(&b, line, HISTORY_TEXT);
    buf_putc(&b, '\n');
    buf_puts(&b, "after\n");
    history_init(&h);
    load(&h, b.data, b.len);
    CHECK(h.count == 2 && strlen(h.items[0]) == HISTORY_TEXT - 1 && is(&h, 1, "after"));
    buf_free(&b);

    /* repeats collapse and the later one wins */
    history_init(&h);
    load_str(&h, "a\nb\na\nc\nb\n");
    CHECK(h.count == 3 && is(&h, 0, "a") && is(&h, 1, "c") && is(&h, 2, "b"));

    /* more lines than the list holds: the newest are kept */
    buf_init(&b);
    for (int i = 0; i < 150; i++) {
        char s[32];
        int k = snprintf(s, sizeof s, "n%d\n", i);
        buf_append(&b, s, (size_t)k);
    }
    history_init(&h);
    load(&h, b.data, b.len);
    CHECK(h.count == HISTORY_MAX && is(&h, 0, "n50") && is(&h, HISTORY_MAX - 1, "n149"));
    buf_free(&b);

    /* loading adds to what is there */
    history_init(&h);
    CHECK(add(&h, "x") && add(&h, "y"));
    load_str(&h, "y\nz\n");
    CHECK(h.count == 3 && is(&h, 0, "x") && is(&h, 1, "y") && is(&h, 2, "z"));

    /* a directory is not a history file */
    mkdir(P("adir"), 0700);
    history_init(&h);
    history_load(&h, P("adir"));
    CHECK(h.count == 0);
}

static void test_load_size_limit(void) {
    /* exactly HISTORY_FILE_MAX bytes is read; one byte more is ignored whole */
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i + 3 <= HISTORY_FILE_MAX; i += 3) buf_puts(&b, "ab\n");
    while (b.len < HISTORY_FILE_MAX) buf_putc(&b, '\n');
    REQUIRE(b.len == HISTORY_FILE_MAX);
    History h;
    history_init(&h);
    load(&h, b.data, b.len);
    CHECK(h.count == 1 && is(&h, 0, "ab"));

    buf_putc(&b, '\n');
    history_init(&h);
    load(&h, b.data, b.len);
    CHECK(h.count == 0);
    buf_free(&b);
}

/* ---- history_save ---- */

static void test_save(void) {
    History h, h2;
    history_init(&h);
    CHECK(add(&h, "a") && add(&h, "b c") && add(&h, "\xc3\xa9"));

    CHECK(history_save(&h, P("s1")));
    CHECK(file_is("s1", "a\nb c\n\xc3\xa9\n"));
    history_init(&h2);
    history_load(&h2, P("s1"));
    CHECK(same(&h, &h2));

    /* private to the user */
    struct stat st;
    REQUIRE(stat(P("s1"), &st) == 0);
    CHECK(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);

    /* no temporary file is left behind */
    CHECK(dir_count(".tmp") == 0);

    /* saving again replaces the content */
    History h3;
    history_init(&h3);
    CHECK(add(&h3, "only"));
    CHECK(history_save(&h3, P("s1")));
    CHECK(file_is("s1", "only\n"));
    CHECK(dir_count(".tmp") == 0);

    /* an empty history gives an empty file */
    history_init(&h3);
    CHECK(history_save(&h3, P("s2")));
    REQUIRE(stat(P("s2"), &st) == 0);
    CHECK(st.st_size == 0);

    /* the full list round-trips */
    history_init(&h);
    for (int i = 0; i < 120; i++) {
        char s[32];
        snprintf(s, sizeof s, "term %d", i);
        add(&h, s);
    }
    CHECK(history_save(&h, P("s3")));
    history_init(&h2);
    history_load(&h2, P("s3"));
    CHECK(h2.count == HISTORY_MAX && same(&h, &h2));
}

static void test_save_failures(void) {
    History h;
    history_init(&h);
    CHECK(add(&h, "a"));

    /* the directory does not exist: fail, and do not create it */
    struct stat st;
    CHECK(!history_save(&h, P("nodir/hist")));
    CHECK(stat(P("nodir"), &st) != 0);

    /* a path too long for the temporary name */
    char longpath[1200];
    int k = snprintf(longpath, sizeof longpath, "%s/", g_dir);
    memset(longpath + k, 'p', 1090);
    longpath[k + 1090] = '\0';
    int before = dir_count("");
    CHECK(!history_save(&h, longpath));
    CHECK(dir_count("") == before);

    /* a directory that cannot be written to (root ignores the mode) */
    if (geteuid() != 0) {
        REQUIRE(mkdir(P("rodir"), 0500) == 0);
        CHECK(!history_save(&h, P("rodir/hist")));
        CHECK(stat(P("rodir/hist"), &st) != 0);
        CHECK(stat(P("rodir"), &st) == 0);
    }

    /* the target is a directory: rename fails, the temporary file goes away */
    REQUIRE(mkdir(P("targetdir"), 0700) == 0);
    CHECK(!history_save(&h, P("targetdir")));
    CHECK(dir_count(".tmp") == 0);
    CHECK(stat(P("targetdir"), &st) == 0 && S_ISDIR(st.st_mode));
}

static const char *tmp_name(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s.%ld.tmp", path, (long)getpid());
    return out;
}

static void test_save_never_clobbers(void) {
    History h;
    history_init(&h);
    CHECK(add(&h, "new"));
    char tmp[1300];

    /* a file already sits at the temporary name: it is left alone */
    write_str("c1", "orig\n");
    tmp_name(P("c1"), tmp, sizeof tmp);
    FILE *f = fopen(tmp, "wb");
    REQUIRE(f != NULL);
    fputs("mine", f);
    fclose(f);
    CHECK(!history_save(&h, P("c1")));
    CHECK(file_is("c1", "orig\n"));
    {
        Buf b;
        buf_init(&b);
        f = fopen(tmp, "rb");
        REQUIRE(f != NULL);
        char tmpb[16];
        size_t n = fread(tmpb, 1, sizeof tmpb, f);
        fclose(f);
        CHECK(n == 4 && memcmp(tmpb, "mine", 4) == 0);
        buf_free(&b);
    }
    unlink(tmp);

    /* a dangling symlink at the temporary name is not followed */
    tmp_name(P("c2"), tmp, sizeof tmp);
    REQUIRE(symlink(P("victim"), tmp) == 0);
    CHECK(!history_save(&h, P("c2")));
    struct stat st;
    CHECK(stat(P("victim"), &st) != 0); /* nothing was created through the link */
    CHECK(stat(P("c2"), &st) != 0);
    unlink(tmp);

    /* a symlink at the history path is replaced, not written through */
    write_str("target", "keep\n");
    REQUIRE(symlink(P("target"), P("link")) == 0);
    CHECK(history_save(&h, P("link")));
    CHECK(file_is("target", "keep\n"));
    REQUIRE(lstat(P("link"), &st) == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK(file_is("link", "new\n"));
    CHECK(dir_count(".tmp") == 0);
}

/* ---- whatever the file contains, what loads is clean and survives a save ---- */

static void test_fuzz(void) {
    static const char BASE[] =
        "alpha\nbeta gamma\n\xc3\xa9\xe6\x97\xa5\nline with spaces\n\ttabbed\nCRLF\r\n"
        "x\nalpha\n\n\x1b[31m\nlast one without newline";
    Buf b;
    buf_init(&b);
    int bad = 0, nonempty = 0;
    for (int it = 0; it < 3000; it++) {
        mutate(&b, BASE, sizeof BASE - 1);
        write_file("fz", b.data, b.len);
        History h, h2;
        history_init(&h);
        history_load(&h, P("fz"));
        if (!valid(&h)) bad++;
        if (h.count > 0) nonempty++;
        if (!history_save(&h, P("fz2"))) bad++;
        history_init(&h2);
        history_load(&h2, P("fz2"));
        if (!same(&h, &h2)) bad++;
    }
    CHECK(bad == 0);
    CHECK(nonempty > 1000);
    CHECK(dir_count(".tmp") == 0);
    buf_free(&b);
}

/* ---- random edits to a list: the invariants always hold ---- */

static void test_add_random(void) {
    History h;
    history_init(&h);
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        char s[HISTORY_TEXT + 8];
        size_t len = rnd() % (HISTORY_TEXT + 4);
        for (size_t k = 0; k < len; k++) {
            /* a small alphabet makes repeats likely; some control bytes sneak in */
            uint32_t r = rnd() % 40;
            char c = (char)('a' + r % 3);
            if (r == 0) c = '\n';
            else if (r == 1) c = '\x01';
            s[k] = c;
        }
        bool ok = add_n(&h, s, len);
        if (!valid(&h)) bad++;
        if (ok && (h.count == 0 || strlen(h.items[h.count - 1]) != len ||
                   memcmp(h.items[h.count - 1], s, len) != 0))
            bad++;
    }
    CHECK(bad == 0);
}

int main(void) {
    printf("test_history\n");
    dir_make();
    test_add();
    test_add_rejects();
    test_add_full();
    test_path();
    test_load();
    test_load_size_limit();
    test_save();
    test_save_failures();
    test_save_never_clobbers();
    test_fuzz();
    test_add_random();
    dir_clean();
    TEST_DONE();
}
