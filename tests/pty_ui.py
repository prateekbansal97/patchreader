import os, pty, sys, time, select, signal, struct, fcntl, termios, subprocess

import tempfile

BIN = os.path.abspath(sys.argv[1])
_tmp = tempfile.NamedTemporaryFile("w", suffix=".patch", delete=False)
for i, name in enumerate(("include/patch.h", "src/a.c", "src/b.c")):
    _tmp.write("diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n@@ -1,2 +1,2 @@\n one\n-two %d\n+three %d\n" % (name, name, name, name, i, i))
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

def key_test(s, send, expect, name):
    s.mark()
    s.send(send)
    check("key " + name, s.wait_for(b"last key: " + expect.encode()), repr(s.out[-120:]))

# ---- 1. keys, resize, quit, restore ----
s = Session([BIN, PATCH])
check("enters alternate screen + hides cursor", s.wait_for(ENTER), repr(s.out[:80]))
check("first frame drawn", s.wait_for(b"last key: (none)"))
check("shows terminal size", b"terminal 80x24" in s.out)
check("shows file count", b"3 file(s)" in s.out)
key_test(s, b"x", "'x'", "plain char")
key_test(s, b"\x1b[A", "Up", "up arrow")
key_test(s, b"\x1b[B", "Down", "down arrow")
key_test(s, b"\x1b[C", "Right", "right arrow")
key_test(s, b"\x1b[D", "Left", "left arrow")
key_test(s, b"\x1bOA", "Up", "SS3 up (application mode)")
key_test(s, b"\x1b[5~", "PageUp", "page up")
key_test(s, b"\x1b[6~", "PageDown", "page down")
key_test(s, b"\x1b[H", "Home", "home (CSI H)")
key_test(s, b"\x1b[1~", "Home", "home (CSI 1~)")
key_test(s, b"\x1bOF", "End", "end (SS3 F)")
key_test(s, b"\x1b[4~", "End", "end (CSI 4~)")
key_test(s, b"\x1b[3~", "Delete", "delete")
key_test(s, b"\x1b[1;5C", "Right", "ctrl+right ignores modifier")
key_test(s, b"\t", "Tab", "tab")
key_test(s, b"\x1b[Z", "Shift-Tab", "shift-tab")
key_test(s, b"\r", "Enter", "enter")
key_test(s, b"\x7f", "Backspace", "backspace")
key_test(s, b"\x04", "Ctrl-d", "ctrl-d")
key_test(s, "é".encode(), "U+00E9", "utf-8 char")
key_test(s, b"\x1b", "Esc", "lone ESC after timeout")
# a sequence split across two writes still decodes as one key
s.mark(); s.send(b"\x1b"); time.sleep(0.01); s.send(b"[B")
check("key split escape sequence", s.wait_for(b"last key: Down"), repr(s.out[-80:]))
# junk escape sequence is swallowed, next key still works
s.mark(); s.send(b"\x1b[999;99~"); time.sleep(0.1); s.send(b"k")
check("key unknown sequence ignored", s.wait_for(b"last key: 'k'"), repr(s.out[-80:]))
# resize
s.mark(); s.resize(30, 100)
check("resize redraws with new size", s.wait_for(b"terminal 100x30"), repr(s.out[-80:]))
# no raw control bytes from content in drawn text other than our own escapes
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

# ---- 2. fatal signals restore the terminal ----
for sig, label in ((signal.SIGTERM, "SIGTERM"), (signal.SIGHUP, "SIGHUP"), (signal.SIGINT, "SIGINT")):
    s = Session([BIN, PATCH])
    s.wait_for(b"last key: (none)")
    s.mark()
    os.kill(s.pid, sig)
    st = s.status()
    s.pump(0.2)
    check(label + " kills the process", st is not None and os.WIFSIGNALED(st) and os.WTERMSIG(st) == sig, str(st))
    check(label + " restores the screen first", LEAVE in s.out, repr(s.out[-60:]))

# Ctrl-C typed on the keyboard
s = Session([BIN, PATCH])
s.wait_for(b"last key: (none)")
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
check("job: UI starts under bash", s.wait_for(b"last key: (none)"), repr(s.out[-80:]))
s.mark(); s.send(b"\x1a")
check("Ctrl-Z: shell reports the job stopped", s.wait_for(b"Stopped"), repr(s.out[-120:]))
check("Ctrl-Z: screen restored before stopping", LEAVE in s.out, repr(s.out[-120:]))
s.mark(); s.send(b"fg\n")
check("fg: re-enters the alternate screen", s.wait_for(ENTER), repr(s.out[-120:]))
check("fg: redraws", s.wait_for(b"last key:"), repr(s.out[-120:]))
key_test(s, b"z", "'z'", "keys work after fg")
s.mark(); s.send(b"\x1a")
check("second Ctrl-Z stops again", s.wait_for(b"Stopped"), repr(s.out[-120:]))
s.mark(); s.send(b"fg\n")
s.wait_for(b"last key:")
s.mark(); s.send(b"q")
check("q after suspend/resume returns to the shell", s.wait_for(LEAVE) and s.wait_for(b"SHELL$ "), repr(s.out[-120:]))
s.send(b"exit\n")
s.status()

# ---- 4. patch on stdin, keys from /dev/tty ----
s = Session(["/bin/sh", "-c", 'cat "%s" | "%s" -' % (PATCH, BIN)])
check("stdin patch: UI starts", s.wait_for(b"3 file(s)"), repr(s.out[-80:]))
key_test(s, b"w", "'w'", "stdin patch: keys come from the tty")
s.send(b"q")
st = s.status()
check("stdin patch: q exits 0", st is not None and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, str(st))

# ---- 5. not a terminal: plain summary, no escape sequences ----
p = subprocess.run([BIN, PATCH], capture_output=True, stdin=subprocess.DEVNULL)
check("piped stdout prints the summary", p.returncode == 0 and b"include/patch.h" in p.stdout and b"\x1b" not in p.stdout, str(p.returncode))
p = subprocess.run([BIN, "--list", PATCH], capture_output=True)
check("--list prints the summary", p.returncode == 0 and p.stdout.count(b"\n") == 3)
s = Session([BIN, "--list", PATCH])
s.wait_for(b"patch.h")
check("--list on a tty does not open the UI", ENTER not in s.out)
s.status()

os.unlink(PATCH)
print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
