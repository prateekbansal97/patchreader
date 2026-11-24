#!/bin/sh
# Source audit and leak check. Run from anywhere: sh tests/audit.sh
# Needs ./patchreader (make) for the leak check.
cd "$(dirname "$0")/.." || exit 1
fail=0

note() { printf '%s\n' "$*"; }
bad() { note "FAIL: $*"; fail=1; }

SRC="src/*.c include/*.h"
# The search history (src/history.c) is the one module allowed to write a
# file. Everything else is held to the read-only rules, and rule 6 limits
# what the history module itself may do.
OTHERS=$(ls src/*.c include/*.h | grep -v '^src/history\.c$')

# 1. unsafe or file-modifying library calls must not appear in the program
hits=$(grep -nE '(^|[^[:alnum:]_])(strcpy|strcat|sprintf|vsprintf|gets|scanf|sscanf|fscanf|system|popen|tmpnam|mktemp)[[:space:]]*\(' $SRC)
[ -z "$hits" ] || bad "unsafe calls:
$hits"

hits=$(grep -nE '(^|[^[:alnum:]_])(unlink|remove|rename|truncate|ftruncate|creat|chmod|chown|mkdir|rmdir|symlink|link|utime|utimes)[[:space:]]*\(' $OTHERS)
[ -z "$hits" ] || bad "calls that modify the file system:
$hits"

# 2. nothing opens a file for writing
hits=$(grep -nE 'O_WRONLY|O_CREAT|O_TRUNC|O_APPEND' $OTHERS)
[ -z "$hits" ] || bad "write-mode open flags:
$hits"
hits=$(grep -nE 'fopen[[:space:]]*\([^)]*"[^"]*[wa+]' $OTHERS)
[ -z "$hits" ] || bad "fopen with a write mode:
$hits"

# 3. the only read-write open is the terminal itself
hits=$(grep -n 'O_RDWR' $SRC | grep -v '/dev/tty')
[ -z "$hits" ] || bad "O_RDWR on something other than /dev/tty:
$hits"

# 4. every file opened by name is opened read-only
hits=$(grep -nE '(^|[^a-z_])(fopen|open)[[:space:]]*\(' $OTHERS | grep -vE '"rb?"|O_RDONLY|/dev/tty')
[ -z "$hits" ] || bad "file opens that are not read-only:
$hits"

# 6. the history module writes one private temporary file and renames it over
#    the history; it never follows a link, removes anything but its own
#    temporary file, or touches any other part of the file system
hits=$(grep -nE '(^|[^[:alnum:]_])open[[:space:]]*\(' src/history.c | grep -v 'O_EXCL')
[ -z "$hits" ] || bad "history.c opens a file without O_EXCL:
$hits"
hits=$(grep -nE '(^|[^[:alnum:]_])open[[:space:]]*\(' src/history.c | grep -v '0600')
[ -z "$hits" ] || bad "history.c creates a file that is not private (0600):
$hits"
hits=$(grep -nE 'O_RDWR|O_TRUNC|O_APPEND' src/history.c)
[ -z "$hits" ] || bad "history.c opens a file in a mode it does not need:
$hits"
hits=$(grep -nE '(^|[^[:alnum:]_])(remove|truncate|ftruncate|creat|chmod|chown|mkdir|rmdir|symlink|link|utime|utimes|fopen|freopen)[[:space:]]*\(' src/history.c)
[ -z "$hits" ] || bad "history.c modifies the file system beyond its temporary file:
$hits"
hits=$(grep -nE '(^|[^[:alnum:]_])(unlink|rename)[[:space:]]*\(' src/history.c | grep -vE '\(tmp[,)]')
[ -z "$hits" ] || bad "history.c removes or renames something other than its temporary file:
$hits"
hits=$(grep -lE '(^|[^[:alnum:]_])history_save[[:space:]]*\(' src/*.c | grep -vE '^src/(history|main)\.c$')
[ -z "$hits" ] || bad "only main.c may save the history, but these call history_save:
$hits"

[ $fail -eq 0 ] && note "ok   source audit"

# 5. leaks, on a sample patch through every non-interactive path
if [ ! -x ./patchreader ]; then
    bad "./patchreader not built (run make first)"
else
    tmp=$(mktemp "${TMPDIR:-/tmp}/audit.XXXXXX") || exit 1
    trap 'rm -f "$tmp"' EXIT
    printf '%s\n' 'diff --git a/a.c b/a.c' '--- a/a.c' '+++ b/a.c' '@@ -1,3 +1,3 @@ f' ' keep' '-old line' '+new line' ' tail' \
        'diff --git a/n b/n' 'new file mode 100644' '--- /dev/null' '+++ b/n' '@@ -0,0 +1 @@' '+x' > "$tmp"
    if command -v leaks >/dev/null 2>&1; then
        for mode in --list --dump; do
            out=$(leaks --atExit -- ./patchreader $mode "$tmp" 2>&1)
            if printf '%s' "$out" | grep -q ' 0 leaks for 0 total leaked bytes'; then
                note "ok   no leaks ($mode)"
            else
                bad "leaks reported for $mode:
$out"
            fi
        done
    elif command -v valgrind >/dev/null 2>&1; then
        for mode in --list --dump; do
            if valgrind -q --leak-check=full --error-exitcode=1 ./patchreader $mode "$tmp" >/dev/null 2>&1; then
                note "ok   no leaks ($mode)"
            else
                bad "valgrind reported problems for $mode"
            fi
        done
    else
        note "skip leak check (neither leaks nor valgrind found)"
    fi
fi

[ $fail -eq 0 ] && note "audit passed"
exit $fail
