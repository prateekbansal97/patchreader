# patchreader

A memory-safe read-only terminal viewer for `.patch` / `.diff` files.

Written in C11.

```
git diff | patchreader -
patchreader some.patch
```

## Build

```
make              # ./patchreader
make asan         # ./patchreader-asan, with AddressSanitizer and UBSan
```

Needs a C11 compiler and a POSIX system. Developed and tested on macOS.

## Use

```
patchreader [options] <patch-file | ->

  --no-color     draw without colors (also set by NO_COLOR)
  --list         print one summary line per file and exit
  --dump         print every file's rows as plain text and exit
  -h, --help     -V, --version
```

When standard output is not a terminal, or with `--list`/`--dump`, it prints
text and exits. `-` reads the patch from standard input. Keys are always read
from the terminal (`/dev/tty`).

Colors are 24-bit when `COLORTERM` is `truecolor` or `24bit`, otherwise the
256-color palette. Without colors, a `+` or `-` in the gutter marks changed
rows and changed words are shown in reverse video.

### Keys

| Keys | Action |
| --- | --- |
| `j` `k`, `Down` `Up`, `Enter` | scroll one line |
| `Space` `b`, `PgDn` `PgUp` | scroll one page |
| `Ctrl-d` `Ctrl-u` | scroll half a page |
| `g` `G`, `Home` `End` | top / bottom |
| `h` `l`, `Left` `Right` | pan sideways; `0` / `$` line start / end |
| `}` `{`, `J` `K` | next / previous hunk |
| `]` `[`, `Tab` `Shift-Tab` | next / previous file |
| `f` | file list (`j`/`k`, `Enter` opens, `Esc` closes) |
| `/` | search the current file (smart case); `n` `N` next / previous match |
| `?` | help |
| `q` | quit |

Ctrl-C and Ctrl-Z work as usual. 

## Supported input

Unified diffs from `git diff`, `git format-patch` (mail headers and commit
message are skipped), `git show` and plain `diff -u`: new, deleted, renamed
and copied files, mode changes, binary files, `\ No newline at end of file`,
quoted file names and CRLF line endings. Hunk line counts are checked, and
malformed input is reported with its line number. Combined diffs (`@@@`, from
`git show` of a merge) are not supported and are refused with a message.

## Memory safety

- Every buffer is length-tracked (`Buf`, `Vec`, `Arena` in `src/util.c`).
  Indexing is bounds-checked (`vec_at` aborts), size arithmetic is
  overflow-checked, and allocation failure aborts instead of returning NULL.
- Limits: 1 GiB of input and 10,000,000 lines per patch. Word marks are
  computed for lines up to 4096 bytes.

## Tests

```
make test    # unit tests, built with ASan and UBSan, including fuzz-style tests
make pty     # drives the real program in a pseudo-terminal (python3)
make audit   # source audit (no unsafe calls, nothing opened for writing) + leak check
make bench   # large-input timing gate
make fuzz    # long-running fuzzer; FUZZ_SECONDS=120 make fuzz
make check   # test, pty, audit and bench
```

`make fuzz` pushes mutated patches through the parser, the display model, the
key handler and the renderer under the sanitizers, and saves a failing input
to `fuzz-crash.diff`.

## Layout

| File | Role |
| --- | --- |
| `src/util.c` | buffers, vectors, arena, checked arithmetic, file reading |
| `src/patch.c` | parser: files, hunks, lines |
| `src/model.c` | one file as screen rows; line numbers; change pairing |
| `src/worddiff.c` | changed words between two lines |
| `src/text.c` | UTF-8, display width, safe text windows |
| `src/search.c` | substring search |
| `src/input.c`, `src/term.c` | key decoder; raw mode, signals, resize |
| `src/nav.c` | viewer state and key handling |
| `src/render.c` | frames: header, gutters, rows, bottom bar, file list, help |
| `src/main.c` | options and the main loop |
