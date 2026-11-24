import os, pty, sys, time, select, signal, struct, fcntl, termios, subprocess

import shutil
import tempfile

BIN = os.path.abspath(sys.argv[1])
_tmp = tempfile.NamedTemporaryFile("w", suffix=".patch", delete=False)
_tmp.write("diff --git a/big.txt b/big.txt\nnew file mode 100644\n--- /dev/null\n+++ b/big.txt\n@@ -0,0 +1,100 @@\n")
for n in range(1, 101):
    _tmp.write("+line %d\n" % n)
for name in ("src/a.c", "src/b.c"):
    _tmp.write("diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n@@ -1,2 +1,2 @@\n one\n-two\n+three\n" % (name, name, name, name))
_tmp.close()
PATCH = _tmp.name
# The program keeps its search history in $HOME: never let these tests touch
# the real one.
_homes = []

def new_home():
    d = tempfile.mkdtemp(prefix="patchreader-home-")
    _homes.append(d)
    os.environ["HOME"] = d
    return d

new_home()
ENTER = b"\x1b[?1049h\x1b[?25l"
LEAVE = b"\x1b[0m\x1b[?25h\x1b[?1049l"
WAVE = b"~" * 8
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + (("  " + extra) if (extra and not cond) else ""))
    if not cond:
        fails += 1

class Session:
    def __init__(self, argv, rows=24, cols=80):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(argv[0], argv)
        self.resize(rows, cols)
        self.out = b""

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def pump(self, t=0.3):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    return
                if not d:
                    return
                self.out += d

    def wait_for(self, text, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.out:
                return True
            self.pump(0.1)
        return text in self.out

    def send(self, data):
        os.write(self.fd, data)

    def mark(self):
        self.out = b""

    def status(self, timeout=3.0, flags=0):
        end = time.time() + timeout
        while time.time() < end:
            pid, st = os.waitpid(self.pid, flags | os.WNOHANG)
            if pid:
                return st
            self.pump(0.05)
        return None

# ---- 1. drawing, scrolling, keys, resize, quit, restore ----
os.environ["COLORTERM"] = "truecolor"
s = Session([BIN, PATCH])
check("enters alternate screen + hides cursor", s.wait_for(ENTER), repr(s.out[:80]))
check("first frame drawn", s.wait_for(b"READ-ONLY"))
check("header shows file count and path", b"1/3 files" in s.out and b"big.txt" in s.out and b"+100 -0" in s.out)
check("hunk row drawn", b"@@ -0,0 +1,100 @@" in s.out)
check("wave row drawn above the hunk row", WAVE in s.out and s.out.index(WAVE) < s.out.index(b"@@ -0,0 +1,100 @@"))
check("wave spans the whole width", b"~" * 80 in s.out and b"~" * 81 not in s.out)
check("added rows are green (truecolor)", b"48;2;36;68;46" in s.out)
check("bottom bar shows hints", b"q quit" in s.out and b"0%" not in s.out.split(b"READ-ONLY")[-1][:3])
check("gutter line numbers", b"line 1" in s.out and b"line 20" in s.out and b"line 21" not in s.out)
check("no content-derived escapes", b"\x1b[31m" not in s.out)

def scrolls_down(send, name, mustnot=WAVE):
    s.mark(); s.send(send)
    check("scroll down: " + name, s.wait_for(b"line 21") and mustnot not in s.out, repr(s.out[-100:]))

def scrolls_up(send, name):
    s.mark(); s.send(send)
    check("scroll up: " + name, s.wait_for(b"@@ -0,0 +1,100 @@") and WAVE in s.out, repr(s.out[-100:]))

for send, name in ((b"j", "j"), (b"\x1b[B", "down arrow"), (b"\x1bOB", "SS3 down"), (b"\r", "enter")):
    scrolls_down(send, name)
    scrolls_up(b"k", "k")
scrolls_down(b"j", "j again")
scrolls_up(b"\x1b[A", "up arrow")
scrolls_down(b"j", "j before SS3 up")
scrolls_up(b"\x1bOA", "SS3 up")

# page keys: 24 rows -> 22 body rows, a page is 21 rows
for send, name in ((b"\x1b[6~", "PageDown"), (b" ", "space")):
    s.mark(); s.send(send)
    check("page down: " + name, s.wait_for(b"line 41") and b"line 42" not in s.out and b"@@ -0" not in s.out and WAVE not in s.out, repr(s.out[-100:]))
    scrolls_up(b"\x1b[5~" if name == "PageDown" else b"b", "page up")
s.mark(); s.send(b"\x04")
check("Ctrl-d scrolls half a page", s.wait_for(b"line 31") and b"line 32" not in s.out)
scrolls_up(b"\x15", "Ctrl-u")

s.mark(); s.send(b"G")
check("G jumps to the end", s.wait_for(b"line 100") and b"100%" in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"j")
check("scrolling past the end stays put", s.wait_for(b"line 100") and b"100%" in s.out)
scrolls_up(b"g", "g")
s.mark(); s.send(b"\x1b[F")
check("End key", s.wait_for(b"line 100"))
scrolls_up(b"\x1b[H", "Home key")

# files: the header follows the file, the view resets to the top
s.mark(); s.send(b"G")
s.wait_for(b"line 100")
for send, name in ((b"]", "]"), (b"\t", "Tab")):
    s.mark(); s.send(send)
    check("next file: " + name, s.wait_for(b"2/3 files") and b"src/a.c" in s.out and b"line 100" not in s.out and b"@@ -1,2 +1,2 @@" in s.out, repr(s.out[-100:]))
    s.mark(); s.send(b"[" if name == "]" else b"\x1b[Z")
    check("previous file: " + name, s.wait_for(b"1/3 files") and b"big.txt" in s.out and b"@@ -0,0 +1,100 @@" in s.out, repr(s.out[-100:]))
s.send(b"]]")
check("last file", s.wait_for(b"3/3 files") and b"src/b.c" in s.out)
s.mark(); s.send(b"]")
s.pump(0.3)
check("next file at the end does nothing", b"4/3" not in s.out)
s.send(b"[[")
s.mark(); s.send(b"g")
s.wait_for(b"1/3 files")

# hunks: the second file below has a single hunk, so use a long one-file jump
s.mark(); s.send(b"}")
s.pump(0.3)
check("} with a single hunk stays", b"line 100" not in s.out)
s.send(b"{")

# file list
s.mark(); s.send(b"f")
check("f opens the file list", s.wait_for(b"Files (3)") and b"big.txt" in s.out and b"src/a.c" in s.out and b"src/b.c" in s.out and b"j/k move" in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[B")
s.pump(0.2)
s.mark(); s.send(b"\r")
check("Enter in the list opens the file", s.wait_for(b"2/3 files") and b"src/a.c" in s.out and b"Files (3)" not in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"f"); time.sleep(0.1); s.send(b"\x1b")
check("Esc closes the list", s.wait_for(b"2/3 files") and b"Files (3)" not in s.out.split(b"2/3 files")[-1], repr(s.out[-100:]))
s.mark(); s.send(b"[")
s.wait_for(b"1/3 files")

# search
s.mark(); s.send(b"/line 50")
check("typing a search shows the prompt", s.wait_for(b"/line 50_"), repr(s.out[-100:]))
s.mark(); s.send(b"\r")
check("search finds the match and scrolls to it", s.wait_for(b"/line 50  1/1") and b"line 50" in s.out and b"@@ -0" not in s.out, repr(s.out[-100:]))
check("match is highlighted", b"48;2;214;178;62" in s.out)
s.mark(); s.send(b"n")
check("n wraps around", s.wait_for(b"(wrapped)"), repr(s.out[-100:]))
s.mark(); s.send(b"/nothing-like-this\r")
check("not found message", s.wait_for(b"Not found: nothing-like-this"), repr(s.out[-100:]))
s.mark(); s.send(b"/abc"); time.sleep(0.1); s.send(b"\x1b")
s.pump(0.3)
check("Esc cancels the prompt", b"/abc_" in s.out and b"q quit" in s.out.split(b"/abc_")[-1], repr(s.out[-100:]))
s.send(b"g")

# help
s.mark(); s.send(b"?")
check("? opens the help", s.wait_for(b"scroll one line") and b"any key closes the help" in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"x")
check("any key closes the help", s.wait_for(b"1/3 files") and b"scroll one line" not in s.out, repr(s.out[-100:]))
s.send(b"g")

# keys with no action must not break anything
for send in (b"x", b"\x1b[1;5C", b"\t", b"\x1b[Z", b"\x7f", b"\x1b[3~", "\u00e9".encode()):
    s.send(send)
s.mark(); s.send(b"\x1b[999;99~"); time.sleep(0.1); s.send(b"G")
check("unknown sequence ignored, next key works", s.wait_for(b"line 100"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1b"); time.sleep(0.2); s.send(b"g")
check("lone ESC is harmless", s.wait_for(b"@@ -0,0 +1,100 @@"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1b"); time.sleep(0.01); s.send(b"[B")
check("split escape sequence scrolls", s.wait_for(b"line 21") and WAVE not in s.out, repr(s.out[-80:]))
s.send(b"g")

# resize: more rows are shown, the header is repainted
s.mark(); s.resize(30, 100)
check("resize redraws with the new size", s.wait_for(b"line 26") and b"line 27" not in s.out and b"1/3 files" in s.out, repr(s.out[-80:]))
s.mark(); s.resize(10, 40)
check("shrinking redraws too", s.wait_for(b"READ-ONLY") and b"line 6" in s.out and b"line 7" not in s.out)
s.resize(24, 80)
s.wait_for(b"line 20")

s.mark(); s.send(b"q")
st = s.status()
check("q exits with status 0", st is not None and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, str(st))
s.pump(0.2)
check("leaves alternate screen + shows cursor on quit", LEAVE in s.out, repr(s.out[-60:]))
try:
    attrs = termios.tcgetattr(s.fd)
    check("tty back in cooked mode", bool(attrs[3] & termios.ICANON) and bool(attrs[3] & termios.ECHO))
except Exception as e:
    print("skip tty-mode check:", e)

# 256-color fallback when COLORTERM is not set
del os.environ["COLORTERM"]
s = Session([BIN, PATCH])
s.wait_for(b"READ-ONLY")
check("256-color fallback", b"48;5;22" in s.out and b"48;2;" not in s.out)
s.send(b"q"); s.status()
os.environ["COLORTERM"] = "truecolor"

# no colors: NO_COLOR and --no-color
def no_color_ok(out):
    return b"48;" not in out and b"38;" not in out and b"\x1b[0;1;7m" in out

os.environ["NO_COLOR"] = "1"
s = Session([BIN, PATCH])
check("NO_COLOR draws without colors", s.wait_for(b"READ-ONLY") and no_color_ok(s.out), repr(s.out[:200]))
check("NO_COLOR marks added rows with a sign", b"+" in s.out and b"@@ -0,0 +1,100 @@" in s.out)
check("NO_COLOR draws the wave dim and plain", b"\x1b[0;2m" + b"~" * 80 in s.out, repr(s.out[:200]))
s.send(b"q"); s.status()
del os.environ["NO_COLOR"]
s = Session([BIN, "--no-color", PATCH])
check("--no-color draws without colors", s.wait_for(b"READ-ONLY") and no_color_ok(s.out), repr(s.out[:200]))
s.send(b"q"); s.status()
os.environ["NO_COLOR"] = ""
s = Session([BIN, PATCH])
check("empty NO_COLOR keeps the colors", s.wait_for(b"READ-ONLY") and b"48;2;36;68;46" in s.out)
s.send(b"q"); s.status()
del os.environ["NO_COLOR"]

# ---- 2. fatal signals restore the terminal ----
for sig, label in ((signal.SIGTERM, "SIGTERM"), (signal.SIGHUP, "SIGHUP"), (signal.SIGINT, "SIGINT")):
    s = Session([BIN, PATCH])
    s.wait_for(b"READ-ONLY")
    s.mark()
    os.kill(s.pid, sig)
    st = s.status()
    s.pump(0.2)
    check(label + " kills the process", st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == sig, str(st))
    check(label + " restores the screen first", LEAVE in s.out, repr(s.out[-60:]))

# a crash must not leave the terminal in raw mode on the alternate screen
for sig, label in ((signal.SIGABRT, "SIGABRT"), (signal.SIGSEGV, "SIGSEGV"), (signal.SIGBUS, "SIGBUS"),
                   (signal.SIGFPE, "SIGFPE"), (signal.SIGILL, "SIGILL")):
    s = Session([BIN, PATCH])
    s.wait_for(b"READ-ONLY")
    s.mark()
    os.kill(s.pid, sig)
    st = s.status()
    s.pump(0.2)
    check(label + " still kills the process", st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == sig, str(st))
    check(label + " restores the screen first", LEAVE in s.out, repr(s.out[-60:]))

# Ctrl-C typed on the keyboard
s = Session([BIN, PATCH])
s.wait_for(b"READ-ONLY")
s.mark(); s.send(b"\x03")
st = s.status()
s.pump(0.2)
check("Ctrl-C exits via SIGINT", st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGINT, str(st))
check("Ctrl-C restores the screen", LEAVE in s.out, repr(s.out[-60:]))

# ---- 3. Ctrl-Z / fg under a real job-control shell ----
os.environ["PS1"] = "SHELL$ "
s = Session(["/bin/bash", "--norc", "--noprofile", "-i"])
s.wait_for(b"SHELL$ ")
s.mark(); s.send(("%s %s\n" % (BIN, PATCH)).encode())
check("job: UI starts under bash", s.wait_for(b"READ-ONLY"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1a")
check("Ctrl-Z: shell reports the job stopped", s.wait_for(b"Stopped"), repr(s.out[-120:]))
check("Ctrl-Z: screen restored before stopping", LEAVE in s.out, repr(s.out[-120:]))
s.mark(); s.send(b"fg\n")
check("fg: re-enters the alternate screen", s.wait_for(ENTER), repr(s.out[-120:]))
check("fg: redraws", s.wait_for(b"READ-ONLY"), repr(s.out[-120:]))
s.mark(); s.send(b"G")
check("keys work after fg", s.wait_for(b"line 100"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1a")
check("second Ctrl-Z stops again", s.wait_for(b"Stopped"), repr(s.out[-120:]))
s.mark(); s.send(b"fg\n")
s.wait_for(b"READ-ONLY")
s.mark(); s.send(b"q")
check("q after suspend/resume returns to the shell", s.wait_for(LEAVE) and s.wait_for(b"SHELL$ "), repr(s.out[-120:]))
s.send(b"exit\n")
s.status()

# ---- 4. patch on stdin, keys from /dev/tty ----
s = Session(["/bin/sh", "-c", 'cat "%s" | "%s" -' % (PATCH, BIN)])
check("stdin patch: UI starts", s.wait_for(b"1/3 files"), repr(s.out[-80:]))
s.mark(); s.send(b"G")
check("stdin patch: keys come from the tty", s.wait_for(b"line 100"), repr(s.out[-80:]))
s.send(b"q")
st = s.status()
check("stdin patch: q exits 0", st is not None and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, str(st))

# ---- 5. not a terminal: plain summary, no escape sequences ----
p = subprocess.run([BIN, PATCH], capture_output=True, stdin=subprocess.DEVNULL)
check("piped stdout prints the summary", p.returncode == 0 and b"big.txt" in p.stdout and b"\x1b" not in p.stdout, str(p.returncode))
p = subprocess.run([BIN, "--list", PATCH], capture_output=True)
check("--list prints the summary", p.returncode == 0 and p.stdout.count(b"\n") == 3)
s = Session([BIN, "--list", PATCH])
s.wait_for(b"big.txt")
check("--list on a tty does not open the UI", ENTER not in s.out)
s.status()

# ---- 6. search history: kept in $HOME/.patchreader_history between runs ----
def read_hist(home):
    try:
        with open(os.path.join(home, ".patchreader_history"), "rb") as f:
            return f.read()
    except OSError:
        return None

def quit_ok(sess):
    sess.send(b"q")
    st = sess.status()
    return st is not None and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0

home = new_home()
hist = os.path.join(home, ".patchreader_history")
check("history: no file before any search", read_hist(home) is None)

s = Session([BIN, PATCH])
s.wait_for(b"READ-ONLY")
s.send(b"/line 5\r")
check("history: a search runs", s.wait_for(b"/line 5  1/11"), repr(s.out[-100:]))
check("history: the search is saved at once", read_hist(home) == b"line 5\n", repr(read_hist(home)))
check("history: the file is private (0600)", (os.stat(hist).st_mode & 0o777) == 0o600, oct(os.stat(hist).st_mode))
s.mark(); s.send(b"/line 9\r")
check("history: a second search", s.wait_for(b"/line 9  1/11"))
check("history: both are saved, newest last", read_hist(home) == b"line 5\nline 9\n", repr(read_hist(home)))
s.mark(); s.send(b"/"); s.send(b"x"); s.send(b"\x7f"); s.send(b"\x1b")
s.pump(0.3)
check("history: Esc records nothing", read_hist(home) == b"line 5\nline 9\n")
check("history: q exits cleanly", quit_ok(s))
check("history: no temporary file is left", os.listdir(home) == [".patchreader_history"], str(os.listdir(home)))

# a new run remembers: Up in the prompt walks back through the searches
s = Session([BIN, PATCH])
s.wait_for(b"READ-ONLY")
s.mark(); s.send(b"/")
s.pump(0.2)
s.mark(); s.send(b"\x1b[A")
check("history: Up recalls the newest search after a restart", s.wait_for(b"/line 9_"), repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[A")
check("history: Up again goes back one more", s.wait_for(b"/line 5_"), repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[A")
s.pump(0.3)
check("history: Up at the oldest stays", b"/line 5_" in s.out and b"/line 9_" not in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[B")
check("history: Down goes forward", s.wait_for(b"/line 9_"), repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[B")
check("history: Down past the newest empties the prompt", s.wait_for(b"/_ "), repr(s.out[-100:]))
s.mark(); s.send(b"x")
s.send(b"\x1b[A")
check("history: typing, then Up shows the newest", s.wait_for(b"/line 9_"), repr(s.out[-100:]))
s.mark(); s.send(b"\x1b[B")
check("history: Down brings back what was typed", s.wait_for(b"/x_"), repr(s.out[-100:]))
s.send(b"\x15")                                   # Ctrl-u clears the prompt
s.mark(); s.send(b"\x1b[A\x1b[A\r")                # recall the older one and search for it
check("history: Enter searches for a recalled entry", s.wait_for(b"/line 5  1/11"), repr(s.out[-100:]))
check("history: a repeated search moves to the end", read_hist(home) == b"line 9\nline 5\n", repr(read_hist(home)))
check("history: q exits cleanly after recalling", quit_ok(s))

# --no-history neither reads nor writes
before = read_hist(home)
s = Session([BIN, "--no-history", PATCH])
s.wait_for(b"READ-ONLY")
s.mark(); s.send(b"/"); s.send(b"\x1b[A")
s.pump(0.5)
check("--no-history: Up finds nothing", b"/_" in s.out and b"/line" not in s.out, repr(s.out[-100:]))
s.send(b"\x1b")
s.mark(); s.send(b"/zzz\r")
check("--no-history: searching still works", s.wait_for(b"Not found: zzz"))
check("--no-history: the file is untouched", read_hist(home) == before, repr(read_hist(home)))
check("--no-history: q exits cleanly", quit_ok(s))
p = subprocess.run([BIN, "--help"], capture_output=True)
check("--help lists --no-history", b"--no-history" in p.stdout)

# two windows at once: neither one throws the other's searches away
home = new_home()
a = Session([BIN, PATCH])
b = Session([BIN, PATCH])
a.wait_for(b"READ-ONLY"); b.wait_for(b"READ-ONLY")
a.send(b"/alpha\r")
check("two windows: first window searches", a.wait_for(b"Not found: alpha"))
b.send(b"/beta\r")
check("two windows: second window searches", b.wait_for(b"Not found: beta"))
check("two windows: both searches are in the file", read_hist(home) == b"alpha\nbeta\n", repr(read_hist(home)))
check("two windows: first quits", quit_ok(a))
check("two windows: second quits", quit_ok(b))

# a damaged file: bad lines are skipped, the rest is used, and the next save cleans it up
home = new_home()
with open(os.path.join(home, ".patchreader_history"), "wb") as f:
    f.write(b"good one\n\x1b[31m\nbad\tline\r\ngood two\n\x00\xff\n" + b"a" * 200 + b"\n\n")
s = Session([BIN, PATCH])
s.wait_for(b"READ-ONLY")
s.mark(); s.send(b"/")
s.pump(0.2)
s.mark(); s.send(b"\x1b[A")
check("damaged history: the newest valid entry is recalled", s.wait_for(b"/good two_"), repr(s.out[-100:]))
check("damaged history: no escape from the file reaches the screen", b"\x1b[31m" not in s.out)
s.mark(); s.send(b"\x15q1\r")
s.wait_for(b"Not found: q1")
check("damaged history: the next save is clean", read_hist(home) == b"good one\ngood two\nq1\n", repr(read_hist(home)))
check("damaged history: q exits cleanly", quit_ok(s))

# a history that cannot be written must not get in the way
if os.geteuid() != 0:
    home = new_home()
    os.chmod(home, 0o500)
    s = Session([BIN, PATCH])
    s.wait_for(b"READ-ONLY")
    s.send(b"/line 5\r")
    check("read-only home: the search still works", s.wait_for(b"/line 5  1/11"), repr(s.out[-100:]))
    s.mark(); s.send(b"/line 9\r")
    check("read-only home: and again", s.wait_for(b"/line 9  1/11"))
    check("read-only home: nothing was written", os.listdir(home) == [], str(os.listdir(home)))
    check("read-only home: q exits cleanly", quit_ok(s))
    os.chmod(home, 0o700)

for bad_home, label in (("relative-home-xyz", "relative HOME"), (None, "no HOME")):
    old = os.environ.get("HOME")
    if bad_home is None:
        del os.environ["HOME"]
    else:
        os.environ["HOME"] = bad_home
    s = Session([BIN, PATCH])
    s.wait_for(b"READ-ONLY")
    s.send(b"/line 5\r")
    check(label + ": the search still works", s.wait_for(b"/line 5  1/11"), repr(s.out[-100:]))
    s.mark(); s.send(b"/"); s.send(b"\x1b[A")
    check(label + ": Up recalls it within the same run", s.wait_for(b"/line 5_"), repr(s.out[-100:]))
    s.send(b"\x1b")
    check(label + ": q exits cleanly", quit_ok(s))
    check(label + ": nothing was created", not os.path.exists("relative-home-xyz") and not os.path.exists(".patchreader_history"))
    os.environ["HOME"] = old

for d in _homes:
    try:
        os.chmod(d, 0o700)
    except OSError:
        pass
    shutil.rmtree(d, ignore_errors=True)
os.unlink(PATCH)
print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
