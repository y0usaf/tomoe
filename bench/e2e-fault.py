import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import time

MAX_OBJECTS = 65536
FLOOD_SYNCS = 35000


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    return s


def sync_msg(new_id):
    return struct.pack("<III", 1, (12 << 16) | 0, new_id)


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.done = set()
        self.error = None
        self.closed = False

    def pump(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return False
        try:
            data = self.sock.recv(1 << 16)
        except (ConnectionResetError, BrokenPipeError):
            self.closed = True
            return True
        if not data:
            self.closed = True
            return True
        self.buf += data
        while len(self.buf) >= 8:
            obj, word = struct.unpack_from("<II", self.buf)
            size = word >> 16
            opcode = word & 0xFFFF
            if size < 8 or len(self.buf) < size:
                break
            if obj == 1 and opcode == 0:
                self.error = self.buf[:size]
            elif obj != 1 and opcode == 0:
                self.done.add(obj)
            self.buf = self.buf[size:]
        return True


def check_max_objects(path):
    sock = connect(path)
    sock.setblocking(True)
    rd = Reader(sock)
    next_id = 2
    while next_id < MAX_OBJECTS:
        end = min(next_id + 512, MAX_OBJECTS)
        sock.sendall(b"".join(sync_msg(i) for i in range(next_id, end)))
        next_id = end
        while rd.pump(0):
            if rd.closed or rd.error:
                raise SystemExit(f"max-objects: rejected early before id {next_id}")
    deadline = time.monotonic() + 10
    while len(rd.done) < MAX_OBJECTS - 2 and time.monotonic() < deadline:
        rd.pump(0.1)
        if rd.closed or rd.error:
            raise SystemExit("max-objects: rejected a legal id")
    if len(rd.done) < MAX_OBJECTS - 2:
        raise SystemExit(f"max-objects: only {len(rd.done)} of {MAX_OBJECTS - 2} legal syncs answered")
    try:
        sock.sendall(sync_msg(MAX_OBJECTS) + sync_msg(MAX_OBJECTS + 1))
    except (BrokenPipeError, ConnectionResetError):
        pass
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not rd.closed and not rd.error:
        rd.pump(0.1)
        if MAX_OBJECTS in rd.done or MAX_OBJECTS + 1 in rd.done:
            raise SystemExit(f"max-objects: FAIL id {MAX_OBJECTS} was accepted")
    if not (rd.closed or rd.error):
        raise SystemExit("max-objects: FAIL no error and no disconnect")
    sock.close()
    return f"max_obj=rejected@{MAX_OBJECTS} legal={MAX_OBJECTS - 2}"


def check_flood(path):
    sock = connect(path)
    sock.settimeout(5)
    payload = b"".join(sync_msg(i) for i in range(2, 2 + FLOOD_SYNCS))
    sent_all = True
    try:
        sock.sendall(payload)
    except (BrokenPipeError, ConnectionResetError):
        sent_all = False
    time.sleep(1.0)
    rd = Reader(sock)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not rd.closed:
        if not rd.pump(0.5):
            break
    sock.close()
    if not rd.closed:
        raise SystemExit(f"flood: FAIL stalled client with {FLOOD_SYNCS * 24} queued reply bytes was not disconnected")
    return f"flood=disconnected sent_all={sent_all}"


def xwm_pids():
    out = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            with open(f"/proc/{e}/stat") as f:
                st = f.read()
        except OSError:
            continue
        if st[st.find("(") + 1:st.rfind(")")] == "tomoe-xwm":
            out.append(int(e))
    return out


def check_xwm_kill(display, client_bin):
    pids = []
    for _ in range(60):
        pids = xwm_pids()
        if pids:
            break
        time.sleep(0.05)
    if len(pids) != 1:
        raise SystemExit(f"xwm: FAIL expected one tomoe-xwm, found {pids}")
    old = pids[0]
    env = dict(os.environ, WAYLAND_DISPLAY=display)
    honest = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "1", "--windows", "1"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.2)
    os.kill(old, signal.SIGKILL)
    new = None
    for _ in range(100):
        cur = [p for p in xwm_pids() if p != old]
        if cur:
            new = cur[0]
            break
        time.sleep(0.05)
    out, _ = honest.communicate(timeout=20)
    if new is None:
        raise SystemExit("xwm: FAIL tomoe-xwm did not respawn")
    if honest.returncode != 0 or not re.search(r"frames [1-9][0-9]*", out) or not re.search(r"lost 0\b", out):
        raise SystemExit(f"xwm: FAIL client disturbed by sidecar kill: {out}")
    return f"xwm {old}->{new}"


FLUSH_CONFIG = """(in-package #:tomoe-user)
(load-policy "desktop")
(define-extension "e2e-flush" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (bind-key '(:mod) "Tab" "focus-next-window")) nil))
"""
FLUSH_SWITCHES = 6
FLUSH_BOUND_MS = 2.0


def check_idle_flush(tomoe_bin, client_bin):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    config = os.path.join(runtime, "e2e-flush.lisp")
    with open(config, "w") as f:
        f.write(FLUSH_CONFIG)
    sock = "e2e-flush"
    tomoe = subprocess.Popen([tomoe_bin, "--socket", sock, "--backend", "headless", "--config", config,
        "--no-watch"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            if subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.05)
        env = dict(os.environ, WAYLAND_DISPLAY=sock, WAYLAND_DEBUG="client")
        client = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "idle", "--seconds",
            str(16 + FLUSH_SWITCHES * 0.3), "--windows", "2"], env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        lines = []
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            ready, _, _ = select.select([client.stderr], [], [], 0.2)
            if not ready:
                continue
            line = client.stderr.readline()
            if not line:
                break
            lines.append(line)
            if re.search(r"wl_keyboard#\d+\.enter", line):
                break
        if not any(re.search(r"wl_keyboard#\d+\.enter", l) for l in lines):
            client.kill()
            raise SystemExit("idle-flush: FAIL no window took keyboard focus within 15s")
        time.sleep(0.2)
        for _ in range(FLUSH_SWITCHES):
            subprocess.run([tomoe_bin, "--socket", sock, "event",
                '(:type :key :owner "e2e-flush" :command "focus-next-window")'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            time.sleep(0.3)
        client.kill()
        _, rest = client.communicate(timeout=30)
        trace = "".join(lines) + rest
    finally:
        tomoe.terminate()
        tomoe.wait(timeout=10)
    events = []
    for line in trace.splitlines():
        m = re.match(r"\[\s*(\d+\.\d+)\].*?(wl_keyboard#\d+\.enter|xdg_toplevel#\d+\.configure\()", line)
        if m:
            events.append((float(m.group(1)), "enter" if "enter" in m.group(2) else "configure"))
    gaps = []
    for i, (t, kind) in enumerate(events):
        if kind != "enter":
            continue
        after = [t2 for t2, k2 in events[i + 1:i + 6] if k2 == "configure"]
        gaps.append(round(after[0] - t, 3) if after else None)
    switched = gaps[-FLUSH_SWITCHES:]
    if len(switched) < FLUSH_SWITCHES or None in switched:
        raise SystemExit(f"idle-flush: FAIL focus switches without a configure: {gaps} "
            f"(trace lines {len(trace.splitlines())}, enters {trace.count('.enter(')}, "
            f"configures {trace.count('.configure(')}, head {trace.splitlines()[:2]})")
    if max(switched) >= FLUSH_BOUND_MS:
        raise SystemExit(f"idle-flush: FAIL configure stranded after keyboard enter (ms): {switched}")
    return f"idle_flush=max{max(switched)}ms"


X11_TIMEOUT = 30


def x_display():
    for _ in range(100):
        try:
            names = sorted(n for n in os.listdir("/tmp/.X11-unix") if n.startswith("X"))
        except OSError:
            names = []
        if names:
            return ":" + names[0][1:]
        time.sleep(0.1)
    raise SystemExit("x11: FAIL tomoe-xwm never created an X socket")


def run_timeout(cmd, env, data=None, timeout=X11_TIMEOUT):
    return subprocess.run(cmd, env=env, input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=timeout)


def clipboard_x_to_wayland(env, size):
    data = os.urandom(size // 4 * 3).hex().encode()[:size]
    owner = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline:
            got = run_timeout(["wl-paste", "--no-newline"], env).stdout
            if got == data:
                return
            time.sleep(0.2)
        raise SystemExit(f"x11: FAIL X->Wayland clipboard of {size} bytes: got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()


def clipboard_wayland_to_x(env, size):
    data = os.urandom(size // 4 * 3).hex().encode()[:size]
    owner = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"],
        env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        got = b""
        while time.monotonic() < deadline:
            got = run_timeout(["xclip", "-selection", "clipboard", "-o",
                "-target", "UTF8_STRING"], env).stdout
            if got == data:
                return
            time.sleep(0.2)
        raise SystemExit(f"x11: FAIL Wayland->X clipboard of {size} bytes: got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()


CUSTOM_MIME = "application/x-tomoe-e2e"


def clipboard_custom(env):
    data = os.urandom(3072).hex().encode()
    owner = subprocess.Popen(["wl-copy", "--foreground", "--type", CUSTOM_MIME], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        got = b""
        while time.monotonic() < deadline:
            got = run_timeout(["xclip", "-selection", "clipboard", "-o", "-target", CUSTOM_MIME],
                env).stdout
            if got == data:
                break
            time.sleep(0.2)
        if got != data:
            raise SystemExit(f"x11: FAIL Wayland->X {CUSTOM_MIME}: got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()
    data = os.urandom(3072).hex().encode()
    owner = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet", "-t", CUSTOM_MIME],
        env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(data)
    owner.stdin.close()
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        got = b""
        types = ""
        while time.monotonic() < deadline:
            types = run_timeout(["wl-paste", "--list-types"], env).stdout.decode(errors="replace")
            if CUSTOM_MIME in types.split():
                got = run_timeout(["wl-paste", "--no-newline", "--type", CUSTOM_MIME], env).stdout
                if got == data:
                    return
            time.sleep(0.2)
        raise SystemExit(f"x11: FAIL X->Wayland {CUSTOM_MIME}: types {types.split()} got {len(got)} bytes")
    finally:
        owner.kill()
        owner.wait()


def clipboard_timeout_recovery(env):
    first = os.urandom(2048).hex().encode()
    second = os.urandom(2048).hex().encode()
    owner_a = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner_a.stdin.write(first)
    owner_a.stdin.close()
    owner_b = None
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline:
            if run_timeout(["wl-paste", "--no-newline"], env).stdout == first:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("x11: FAIL timeout-recovery owner A never pasted")
        os.kill(owner_a.pid, signal.SIGSTOP)
        started = time.monotonic()
        try:
            run_timeout(["wl-paste", "--no-newline"], env, timeout=20)
        except subprocess.TimeoutExpired:
            raise SystemExit("x11: FAIL paste from a stalled X owner did not time out within 20s")
        stalled = time.monotonic() - started
        owner_b = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet"], env=env,
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        owner_b.stdin.write(second)
        owner_b.stdin.close()
        time.sleep(0.3)
        os.kill(owner_a.pid, signal.SIGCONT)
        good = 0
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline and good < 5:
            got = run_timeout(["wl-paste", "--no-newline"], env).stdout
            if got == second:
                good += 1
            elif got == first:
                raise SystemExit("x11: FAIL a stale reply from the stalled owner reached a later paste")
            time.sleep(0.1)
        if good < 5:
            raise SystemExit("x11: FAIL pastes did not recover after a conversion timeout")
        return f"timeout-recovery(stall {stalled:.1f}s)"
    finally:
        for owner in (owner_a, owner_b):
            if owner:
                try:
                    os.kill(owner.pid, signal.SIGCONT)
                except OSError:
                    pass
                owner.kill()
                owner.wait()


def check_x11(path, tomoe_bin):
    sock = os.path.basename(path)
    env = dict(os.environ, WAYLAND_DISPLAY=sock, DISPLAY=x_display())
    logo = subprocess.Popen(["xlogo", "-geometry", "240x120"], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        mapped = False
        while time.monotonic() < deadline and not mapped:
            out = subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True).stdout
            mapped = "xlogo" in out.lower()
            if not mapped:
                time.sleep(0.2)
        if not mapped:
            raise SystemExit("x11: FAIL xlogo never appeared in tomoe inspect")
        clipboard_x_to_wayland(env, 4096)
        clipboard_x_to_wayland(env, 24 << 20)
        clipboard_wayland_to_x(env, 4096)
        clipboard_wayland_to_x(env, 2 << 20)
        clipboard_custom(env)
        recovery = clipboard_timeout_recovery(env)
    finally:
        logo.kill()
        logo.wait()
    return f"x11=mapped,clip-x2w(4K,24M),clip-w2x(4K,2M),custom-mime-both,{recovery}"


def check_x11_stale_reply(path):
    sock = os.path.basename(path)
    env = dict(os.environ, WAYLAND_DISPLAY=sock, DISPLAY=x_display())
    logo = subprocess.Popen(["xlogo"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = os.urandom(1024).hex().encode()
    owner_a = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner_a.stdin.write(text)
    owner_a.stdin.close()
    owners = [owner_a]
    paste = None
    try:
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline:
            if run_timeout(["wl-paste", "--no-newline"], env).stdout == text:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("x11-stale: FAIL owner A never pasted")
        os.kill(owner_a.pid, signal.SIGSTOP)
        stalled_at = time.monotonic()
        paste = subprocess.Popen(["wl-paste", "--no-newline"], env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        owner_b = subprocess.Popen(["xclip", "-selection", "clipboard", "-i", "-quiet", "-t",
            CUSTOM_MIME], env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        owners.append(owner_b)
        owner_b.stdin.write(b"custom")
        owner_b.stdin.close()
        time.sleep(0.5)
        os.kill(owner_b.pid, signal.SIGSTOP)
        time.sleep(max(0.0, stalled_at + 5.6 - time.monotonic()))
        os.kill(owner_a.pid, signal.SIGCONT)
        time.sleep(0.7)
        os.kill(owner_b.pid, signal.SIGCONT)
        deadline = time.monotonic() + 15
        types = []
        while time.monotonic() < deadline:
            types = run_timeout(["wl-paste", "--list-types"], env).stdout.decode(errors="replace").split()
            if CUSTOM_MIME in types:
                return "x11_stale_reply=ignored"
            time.sleep(0.2)
        raise SystemExit(f"x11-stale: FAIL adoption of owner B took a stale reply; offered {types}")
    finally:
        for owner in owners:
            try:
                os.kill(owner.pid, signal.SIGCONT)
            except OSError:
                pass
            owner.kill()
            owner.wait()
        if paste:
            paste.kill()
            paste.wait()
        logo.kill()
        logo.wait()


def check_x11_ownership(path):
    sock = os.path.basename(path)
    env = dict(os.environ, WAYLAND_DISPLAY=sock, DISPLAY=x_display())
    logo = subprocess.Popen(["xlogo"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    z_text = os.urandom(1024).hex().encode()
    x_text = os.urandom(1024).hex().encode()
    w_text = os.urandom(1024).hex().encode()
    procs = []

    def start(cmd, data):
        proc = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        proc.stdin.write(data)
        proc.stdin.close()
        procs.append(proc)
        return proc

    try:
        owner_z = start(["xclip", "-selection", "clipboard", "-i", "-quiet"], z_text)
        deadline = time.monotonic() + X11_TIMEOUT
        while time.monotonic() < deadline:
            if run_timeout(["wl-paste", "--no-newline"], env).stdout == z_text:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("x11-ownership: FAIL owner Z never pasted")
        os.kill(owner_z.pid, signal.SIGSTOP)
        stalled_at = time.monotonic()
        stalled = subprocess.Popen(["wl-paste", "--no-newline"], env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        procs.append(stalled)
        time.sleep(0.5)
        start(["xclip", "-selection", "clipboard", "-i", "-quiet"], x_text)
        time.sleep(0.5)
        start(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"], w_text)
        time.sleep(max(0.0, stalled_at + 6.5 - time.monotonic()))
        for attempt in range(5):
            got = run_timeout(["wl-paste", "--no-newline"], env).stdout
            if got != w_text:
                kind = "X data" if got == x_text else "Z data" if got == z_text else "other"
                raise SystemExit("x11-ownership: FAIL a stale X adoption replaced the Wayland selection "
                    f"(paste {attempt}: {len(got)} bytes, {kind})")
            time.sleep(0.1)
        return "x11_ownership=kept"
    finally:
        for proc in procs:
            try:
                os.kill(proc.pid, signal.SIGCONT)
            except OSError:
                pass
            proc.kill()
            proc.wait()
        logo.kill()
        logo.wait()

def check_xwm_hang(path, client_bin, tomoe_bin):
    sock = os.path.basename(path)
    env = dict(os.environ, WAYLAND_DISPLAY=sock)
    pids = []
    for _ in range(100):
        pids = xwm_pids()
        if pids:
            break
        time.sleep(0.05)
    if len(pids) != 1:
        raise SystemExit(f"xwm-hang: FAIL expected one tomoe-xwm, found {pids}")
    old = pids[0]
    honest = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "6",
        "--windows", "1"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.3)
    os.kill(old, signal.SIGSTOP)
    stopped_at = time.monotonic()
    owner = subprocess.Popen(["wl-copy", "--foreground"], env=env, stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    owner.stdin.write(b"hang-probe")
    owner.stdin.close()
    new = None
    try:
        while time.monotonic() - stopped_at < 10:
            current = [p for p in xwm_pids() if p != old]
            if current:
                new = current[0]
                break
            time.sleep(0.05)
        if new is None:
            raise SystemExit("xwm-hang: FAIL a stopped tomoe-xwm was not replaced within 10s")
        restarted = time.monotonic() - stopped_at
        out, _ = honest.communicate(timeout=30)
        if honest.returncode != 0 or not re.search(r"frames [1-9][0-9]*", out) or not re.search(r"lost 0\b", out):
            raise SystemExit(f"xwm-hang: FAIL Wayland client disturbed by the sidecar hang: {out[-200:]}")
        xenv = dict(env, DISPLAY=x_display())
        logo = subprocess.Popen(["xlogo"], env=xenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + X11_TIMEOUT
            while time.monotonic() < deadline:
                out = subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL, text=True).stdout
                if "xlogo" in out.lower():
                    break
                time.sleep(0.2)
            else:
                raise SystemExit("xwm-hang: FAIL X clients do not map after the sidecar restart")
        finally:
            logo.kill()
            logo.wait()
        return f"xwm_hang={old}->{new} in {restarted:.1f}s"
    finally:
        try:
            os.kill(old, signal.SIGCONT)
        except OSError:
            pass
        owner.kill()
        owner.wait()
        if honest.poll() is None:
            honest.kill()
            honest.wait()


def parent_pid(pid):
    with open(f"/proc/{pid}/stat") as f:
        return int(f.read().rsplit(")", 1)[1].split()[1])


def check_xwm_stall(path):
    env = dict(os.environ, WAYLAND_DISPLAY=os.path.basename(path))
    pids = []
    for _ in range(100):
        pids = xwm_pids()
        if pids:
            break
        time.sleep(0.05)
    if len(pids) != 1:
        raise SystemExit(f"xwm-stall: FAIL expected one tomoe-xwm, found {pids}")
    sidecar = pids[0]
    compositor = parent_pid(sidecar)
    owner = None
    try:
        os.kill(sidecar, signal.SIGSTOP)
        owner = subprocess.Popen(["wl-copy", "--foreground"], env=env, stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        owner.stdin.write(b"stall-probe")
        owner.stdin.close()
        time.sleep(0.5)
        os.kill(compositor, signal.SIGSTOP)
        os.kill(sidecar, signal.SIGCONT)
        time.sleep(3.5)
        os.kill(compositor, signal.SIGCONT)
        time.sleep(2.0)
        after = xwm_pids()
        if after != [sidecar]:
            raise SystemExit(f"xwm-stall: FAIL a sidecar whose pong was already queued was restarted "
                f"({sidecar} -> {after})")
        return f"xwm_stall=kept {sidecar}"
    finally:
        for pid in (compositor, sidecar):
            try:
                os.kill(pid, signal.SIGCONT)
            except OSError:
                pass
        if owner:
            owner.kill()
            owner.wait()


def main():
    args = sys.argv[1:]
    checks = args[0].split(",")
    path = args[1]
    display = os.path.basename(path)
    client_bin = args[2] if len(args) > 2 else None
    results = []
    for check in checks:
        if check == "max-objects":
            results.append(check_max_objects(path))
        elif check == "flood":
            results.append(check_flood(path))
        elif check == "xwm":
            results.append(check_xwm_kill(display, client_bin))
        elif check == "xwm-stall":
            results.append(check_xwm_stall(path))
        elif check == "xwm-hang":
            results.append(check_xwm_hang(path, client_bin, args[3]))
        elif check == "x11-ownership":
            results.append(check_x11_ownership(path))
        elif check == "x11-stale":
            results.append(check_x11_stale_reply(path))
        elif check == "x11":
            results.append(check_x11(path, args[3]))
        elif check == "idle-flush":
            results.append(check_idle_flush(args[3], client_bin))
        else:
            raise SystemExit(f"unknown check {check}")
    print("e2e-fault-isolation: " + " ".join(results))


main()
