import os, pty, sys, time, select, signal, struct, fcntl, termios, subprocess

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
ENTER = b"\x1b[?1049h\x1b[?25l"
LEAVE = b"\x1b[0m\x1b[?25h\x1b[?1049l"
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
check("added rows are green (truecolor)", b"48;2;36;68;46" in s.out)
check("bottom bar shows hints", b"q quit" in s.out and b"0%" not in s.out.split(b"READ-ONLY")[-1][:3])
check("gutter line numbers", b"line 1" in s.out and b"line 21" in s.out and b"line 22" not in s.out)
check("no content-derived escapes", b"\x1b[31m" not in s.out)

def scrolls_down(send, name, mustnot=b"@@ -0"):
    s.mark(); s.send(send)
    check("scroll down: " + name, s.wait_for(b"line 22") and mustnot not in s.out, repr(s.out[-100:]))

def scrolls_up(send, name):
    s.mark(); s.send(send)
    check("scroll up: " + name, s.wait_for(b"@@ -0,0 +1,100 @@"), repr(s.out[-100:]))

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
    check("page down: " + name, s.wait_for(b"line 42") and b"line 43" not in s.out and b"@@ -0" not in s.out, repr(s.out[-100:]))
    scrolls_up(b"\x1b[5~" if name == "PageDown" else b"b", "page up")
s.mark(); s.send(b"\x04")
check("Ctrl-d scrolls half a page", s.wait_for(b"line 32") and b"line 33" not in s.out)
scrolls_up(b"\x15", "Ctrl-u")

s.mark(); s.send(b"G")
check("G jumps to the end", s.wait_for(b"line 100") and b"100%" in s.out, repr(s.out[-100:]))
s.mark(); s.send(b"j")
check("scrolling past the end stays put", s.wait_for(b"line 100") and b"100%" in s.out)
scrolls_up(b"g", "g")
s.mark(); s.send(b"\x1b[F")
check("End key", s.wait_for(b"line 100"))
scrolls_up(b"\x1b[H", "Home key")

# keys with no action must not break anything
for send in (b"x", b"\x1b[1;5C", b"\t", b"\x1b[Z", b"\x7f", b"\x1b[3~", "\u00e9".encode()):
    s.send(send)
s.mark(); s.send(b"\x1b[999;99~"); time.sleep(0.1); s.send(b"G")
check("unknown sequence ignored, next key works", s.wait_for(b"line 100"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1b"); time.sleep(0.2); s.send(b"g")
check("lone ESC is harmless", s.wait_for(b"@@ -0,0 +1,100 @@"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1b"); time.sleep(0.01); s.send(b"[B")
check("split escape sequence scrolls", s.wait_for(b"line 22") and b"@@ -0" not in s.out, repr(s.out[-80:]))
s.send(b"g")

# resize: more rows are shown, the header is repainted
s.mark(); s.resize(30, 100)
check("resize redraws with the new size", s.wait_for(b"line 27") and b"1/3 files" in s.out, repr(s.out[-80:]))
s.mark(); s.resize(10, 40)
check("shrinking redraws too", s.wait_for(b"READ-ONLY") and b"line 7" in s.out and b"line 8" not in s.out)
s.resize(24, 80)
s.wait_for(b"line 21")

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

os.unlink(PATCH)
print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
