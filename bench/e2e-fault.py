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


def proc_children(pid, name):
    out = []
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            with open(f"/proc/{e}/stat") as f:
                st = f.read()
        except OSError:
            continue
        comm = st[st.find("(") + 1:st.rfind(")")]
        ppid = int(st[st.rfind(")") + 2:].split()[1])
        if comm == name and ppid == pid:
            out.append(int(e))
    return out


def inspect(tomoe_bin, sock, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        if r.returncode == 0 and r.stdout.startswith("(2 :OK"):
            return r.stdout
        time.sleep(0.05)
    raise SystemExit(f"policy: FAIL inspect did not answer on {sock}")


def scene(text):
    apps = {int(i): a for i, a in re.findall(r'\(:ID (\d+) :IDENTIFIER "[^"]*" :TITLE "[^"]*" :APP-ID "([^"]*)"', text)}
    m = re.search(r":LAYOUT (\(.*?\)|NIL) :STACKING \(?([^)]*?)\)? :LAYERS.*?:FOCUS (\d+|NIL)", text, re.S)
    if not m:
        raise SystemExit(f"policy: FAIL cannot read the scene: {text[:300]}")
    layout = {}
    for i, x, y, w, h, v in re.findall(r"\(:ID (\d+) :X (-?\d+) :Y (-?\d+) :WIDTH (\d+) :HEIGHT (\d+) :VISIBLE (T|NIL)", m.group(1)):
        layout[apps.get(int(i), i)] = (int(x), int(y), int(w), int(h), v == "T")
    stacking = [apps.get(int(i), i) for i in m.group(2).split() if i != "NIL"]
    focus = apps.get(int(m.group(3)), m.group(3)) if m.group(3) != "NIL" else None
    return {"layout": layout, "stacking": stacking, "focus": focus}


def wait_windows(tomoe_bin, sock, count, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sc = scene(inspect(tomoe_bin, sock))
        if len(sc["layout"]) == count:
            return sc
        time.sleep(0.1)
    raise SystemExit(f"policy: FAIL expected {count} windows, have {sc['layout']}")


def start_tomoe(tomoe_bin, sock, config, extra=()):
    tomoe = subprocess.Popen([tomoe_bin, "--socket", sock, "--backend", "headless", "--config", config,
        "--no-watch", *extra], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    inspect(tomoe_bin, sock)
    return tomoe


def policy_pid(tomoe, old=None, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pids = [p for p in proc_children(tomoe.pid, "tomoe-policy") if p != old]
        if len(pids) == 1:
            return pids[0]
        time.sleep(0.02)
    raise SystemExit(f"policy: FAIL no tomoe-policy (old {old})")


def command(tomoe_bin, sock, *args):
    subprocess.run([tomoe_bin, "--socket", sock, "command", *args], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, check=True, timeout=10)


KEY_SUPER, KEY_J, KEY_Q = 125 - 8, 36 - 8, 16 - 8

POLICY_BINDINGS = """
(define-extension "e2e-floor" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (define-command "focus-next-window" :description "Focus the next window.")
                      (define-command "close-window" :description "Close the focused window.")
                      (bind-key '(:super) "q" "close-window")
                      (bind-key '(:super) "j" "focus-next-window")) nil))
"""


def client(client_bin, sock, tag, windows, chords=None):
    args = [client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "60", "--windows",
        str(windows), "--tag", tag]
    if chords:
        args += ["--chords", chords]
    return subprocess.Popen(args, env=dict(os.environ, WAYLAND_DISPLAY=sock), stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, bufsize=0)


READ_BUFFERS = {}


def read_until(proc, pattern, timeout):
    deadline = time.monotonic() + timeout
    seen = []
    buffer = READ_BUFFERS.setdefault(proc.pid, bytearray())
    while time.monotonic() < deadline:
        while b"\n" in buffer:
            line, _, rest = bytes(buffer).partition(b"\n")
            buffer[:] = rest
            text = line.decode(errors="replace").strip()
            seen.append(text)
            if re.search(pattern, text):
                return seen
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if not ready:
            continue
        chunk = os.read(proc.stdout.fileno(), 4096)
        if not chunk:
            break
        buffer += chunk
    raise SystemExit(f"policy: FAIL client never printed {pattern!r}: {seen}")


def check_policy_kill(tomoe_bin, client_bin, workspaces):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    config = os.path.join(runtime, "e2e-policy.lisp")
    with open(workspaces) as f:
        text = f.read()
    with open(config, "w") as f:
        f.write(text + POLICY_BINDINGS)
    sock = "e2e-policy"
    chords = os.path.join(runtime, "e2e-policy.chords")
    tomoe = start_tomoe(tomoe_bin, sock, config, ["--test-policy-boot-delay=1500"])
    clients = []
    try:
        command(tomoe_bin, sock, "switch-tag", "tag=2")
        clients.append(client(client_bin, sock, "a", 1))
        wait_windows(tomoe_bin, sock, 1)
        command(tomoe_bin, sock, "switch-tag", "tag=3")
        keyed = client(client_bin, sock, "b", 2, chords)
        clients.append(keyed)
        before = wait_windows(tomoe_bin, sock, 3)
        time.sleep(0.6)
        before = scene(inspect(tomoe_bin, sock))
        visible = sorted(a for a, g in before["layout"].items() if g[4])
        if visible != ["bench-b-0", "bench-b-1"] or before["layout"]["bench-a-0"][4]:
            raise SystemExit(f"policy: FAIL tags not set up: {before}")
        pid = policy_pid(tomoe)
        kills = []
        for round in range(3):
            os.kill(pid, signal.SIGKILL)
            killed = time.monotonic()
            if round == 0:
                with open(chords + ".tmp", "w") as f:
                    f.write(f"{KEY_SUPER}+{KEY_J}\n")
                os.rename(chords + ".tmp", chords)
                lines = read_until(keyed, r"chords done", 5)
                with open(chords + ".tmp", "w") as f:
                    f.write(f"{KEY_SUPER}+{KEY_Q}\n")
                os.rename(chords + ".tmp", chords)
                lines += read_until(keyed, r"close \d", 5)
                floor_ms = (time.monotonic() - killed) * 1000
                if not any(l.startswith("enter") for l in lines) or floor_ms > 1400:
                    raise SystemExit(f"policy: FAIL emergency bindings while down ({floor_ms:.0f} ms): {lines}")
            if tomoe.poll() is not None:
                raise SystemExit("policy: FAIL tomoe exited when its policy was killed")
            new = policy_pid(tomoe, pid)
            after = scene(inspect(tomoe_bin, sock, 20))
            kills.append(f"{pid}->{new}:{(time.monotonic() - killed):.1f}s")
            if after != before:
                raise SystemExit(f"policy: FAIL scene changed across kill {round}: {before} != {after}")
            pid = new
            time.sleep(0.4)
        os.kill(pid, signal.SIGKILL)
        killed = time.monotonic()
        late = client(client_bin, sock, "c", 1)
        clients.append(late)
        clients[0].kill()
        clients[0].wait()
        new = policy_pid(tomoe, pid)
        deadline = time.monotonic() + 20
        while True:
            raced = scene(inspect(tomoe_bin, sock, 20))
            if sorted(raced["layout"]) == ["bench-b-0", "bench-b-1", "bench-c-0"] or time.monotonic() > deadline:
                break
            time.sleep(0.1)
        if sorted(raced["layout"]) != ["bench-b-0", "bench-b-1", "bench-c-0"] or \
                not raced["layout"]["bench-c-0"][4] or not raced["layout"]["bench-b-0"][4]:
            raise SystemExit(f"policy: FAIL race across resync: {raced}")
        keyed.kill()
        keyed.wait(timeout=10)
        return f"policy-kill {' '.join(kills)} race=ok"
    finally:
        for c in clients:
            if c.poll() is None:
                c.kill()
                c.wait()
        tomoe.terminate()
        tomoe.wait(timeout=10)


EFFECTS_CONFIG = """(in-package #:tomoe-user)
(define-extension "e2e-effects" (:reads (:command) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (let ((type (getf event :type)) (name (getf event :name)))
    (values state
            (list (once :boot 0) (interval :tick 200)
                  (once :later 1500)
{extra}
                  (define-command "e2e-launch" :description "Launch once.")
                  (define-command "e2e-random" :description "Record a random draw.")
                  (bind-key '(:super) "n" "e2e-launch"))
            (append (when (eq type :exec)
                      (list (spawn "echo exec >> {execs}")))
                    (when (eq type :watch)
                      (list (spawn "echo watch >> {watches}")))
                    (when (and (eq type :timer) (eq name :later))
                      (list (spawn "echo later >> {later}")))
                    (when (and (eq type :timer) (eq name :boot))
                      (list (spawn "echo launch >> {launch}")))
                    (when (and (eq type :timer) (eq name :tick))
                      (list (spawn "echo tick >> {tick}")))
                    (when (and (eq type :command) (equal name "e2e-launch"))
                      (list (spawn "echo new >> {fresh}")))
                    (when (and (eq type :command) (equal name "e2e-random"))
                      (list (spawn (format nil "echo ~D >> {draws}" (random 1000000)))))))))
"""
KEY_N = 49 - 8


def lines_of(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except OSError:
        return []


def wait_lines(path, count, timeout):
    deadline = time.monotonic() + timeout
    while len(lines_of(path)) < count and time.monotonic() < deadline:
        time.sleep(0.005)
    return len(lines_of(path))


class Effects:
    def __init__(self, tomoe_bin, tag, flags, extra=""):
        runtime = os.environ["XDG_RUNTIME_DIR"]
        self.tomoe_bin = tomoe_bin
        self.sock = f"e2e-fx-{tag}"
        self.files = {k: os.path.join(runtime, f"e2e-fx-{tag}.{k}") for k in ("launch", "tick", "fresh", "draws", "later", "execs", "watches", "watched")}
        for path in self.files.values():
            if os.path.exists(path):
                os.unlink(path)
        config = os.path.join(runtime, f"e2e-fx-{tag}.lisp")
        with open(config, "w") as f:
            f.write(EFFECTS_CONFIG.format(extra=extra.format(**self.files), **self.files))
        self.tomoe = subprocess.Popen([tomoe_bin, "--socket", self.sock, "--backend", "headless",
            "--config", config, "--no-watch", *flags], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.pid = None

    def policy(self, old=None):
        return policy_pid(self.tomoe, old)

    def kill(self):
        old = self.policy()
        os.kill(old, signal.SIGKILL)
        new = self.policy(old)
        inspect(self.tomoe_bin, self.sock, 20)
        return f"{old}->{new}"

    def timers(self):
        found = re.findall(r":TIMERS \((.*?)\) :WATCHES", inspect(self.tomoe_bin, self.sock, 20), re.S)
        return found[0] if found else None

    def command(self, name):
        command(self.tomoe_bin, self.sock, name)

    def close(self):
        self.tomoe.terminate()
        self.tomoe.wait(timeout=10)


def policy_generation(tomoe_bin, sock):
    out = subprocess.run([tomoe_bin, "--socket", sock, "memory"], stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True).stdout
    found = re.search(r":POLICY-GENERATION (\d+)", out)
    return int(found.group(1)) if found else 0


def effects_after_crash(fx):
    deadline = time.monotonic() + 15
    while policy_generation(fx.tomoe_bin, fx.sock) < 2 and time.monotonic() < deadline:
        time.sleep(0.05)
    generation = policy_generation(fx.tomoe_bin, fx.sock)
    if generation < 2:
        raise SystemExit("policy-effects: FAIL the policy never crashed at the test hook")
    return f"generation={generation}"


def effects_duplicate(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "dup", ["--test-policy-crash-before-done", "--test-policy-snapshot-hold", *extra])
    try:
        kills = effects_after_crash(fx)
        time.sleep(1.0)
        launched = len(lines_of(fx.files["launch"]))
        if launched != 1:
            raise SystemExit(f"policy-effects: FAIL accepted launch ran {launched} times after a crash before DONE")
        timers = fx.timers() or ""
        if len(re.findall(r"\(:OWNER", timers)) != 3 or ':NAME :BOOT :KIND :ONCE :MILLISECONDS 0 :STATUS :FIRED' not in timers \
                or ':NAME :TICK :KIND :INTERVAL :MILLISECONDS 200 :STATUS :PENDING' not in timers:
            raise SystemExit(f"policy-effects: FAIL timers after resync: {timers}")
        start = len(lines_of(fx.files["tick"]))
        time.sleep(2.0)
        rate = (len(lines_of(fx.files["tick"])) - start) / 2.0
        if not 3.5 <= rate <= 6.5:
            raise SystemExit(f"policy-effects: FAIL timer fires {rate}/s after resync, not 5/s")
        return f"dup:launches=1,timers=kept,rate={rate}/s,{kills}"
    finally:
        fx.close()


def effects_lost(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "lost", ["--test-policy-crash-before-flush", *extra])
    try:
        kills = effects_after_crash(fx)
        wait_lines(fx.files["launch"], 1, 5)
        time.sleep(1.0)
        launched = len(lines_of(fx.files["launch"]))
        if launched != 1:
            raise SystemExit(f"policy-effects: FAIL launch lost in a crash before it reached the core ran {launched} times")
        return f"lost:launches=1,{kills}"
    finally:
        fx.close()


def effects_pending(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "pending", list(extra))
    try:
        if wait_lines(fx.files["launch"], 1, 10) != 1:
            raise SystemExit("policy-effects: FAIL the one-shot launch never ran")
        old = fx.policy()
        os.kill(old, signal.SIGSTOP)
        time.sleep(2.0)
        if lines_of(fx.files["later"]):
            raise SystemExit("policy-effects: FAIL the later timer ran while the policy was stopped")
        os.kill(old, signal.SIGKILL)
        new = fx.policy(old)
        inspect(tomoe_bin, fx.sock, 20)
        wait_lines(fx.files["later"], 1, 5)
        time.sleep(1.0)
        later = len(lines_of(fx.files["later"]))
        if later != 1:
            raise SystemExit(f"policy-effects: FAIL a timer that fired while the policy was stopped ran {later} times")
        return f"pending:later=1,{old}->{new}"
    finally:
        fx.close()


def effects_downtime(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "down", ["--test-policy-restart-delay=2500", *extra],
        '(exec-async :job "sleep 1.5; echo done") (watch-file :watched "{watched}")')
    try:
        with open(fx.files["watched"], "w") as f:
            f.write("zero\n")
        inspect(tomoe_bin, fx.sock, 20)
        time.sleep(0.4)
        watches = len(lines_of(fx.files["watches"]))
        if lines_of(fx.files["execs"]):
            raise SystemExit("policy-effects: FAIL the exec finished before the policy was killed")
        old = fx.policy()
        os.kill(old, signal.SIGKILL)
        time.sleep(0.3)
        with open(fx.files["watched"], "w") as f:
            f.write("one\n")
        time.sleep(0.2)
        with open(fx.files["watched"], "w") as f:
            f.write("two\n")
        time.sleep(1.5)
        if lines_of(fx.files["execs"]) or len(lines_of(fx.files["watches"])) != watches:
            raise SystemExit("policy-effects: FAIL callbacks ran while no policy was running")
        new = policy_pid(fx.tomoe, old, 10)
        inspect(tomoe_bin, fx.sock, 20)
        wait_lines(fx.files["execs"], 1, 5)
        time.sleep(1.0)
        execs = len(lines_of(fx.files["execs"]))
        changed = len(lines_of(fx.files["watches"])) - watches
        if execs != 1 or changed != 1:
            raise SystemExit(f"policy-effects: FAIL while the policy was down: exec callback ran {execs} times, "
                f"watch callback {changed} times (want 1 and 1)")
        return f"downtime:exec=1,watch=1,{old}->{new}"
    finally:
        fx.close()


def effects_overflow(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "full", ["--test-policy-ring=16", *extra],
        '(exec-async :job "sleep 1.5; echo done")')
    try:
        inspect(tomoe_bin, fx.sock, 20)
        time.sleep(0.4)
        if lines_of(fx.files["execs"]):
            raise SystemExit("policy-effects: FAIL the exec finished before the policy was stopped")
        old = fx.policy()
        os.kill(old, signal.SIGSTOP)
        new = policy_pid(fx.tomoe, old, 20)
        inspect(tomoe_bin, fx.sock, 20)
        wait_lines(fx.files["execs"], 1, 5)
        time.sleep(1.0)
        execs = len(lines_of(fx.files["execs"]))
        if execs != 1:
            raise SystemExit(f"policy-effects: FAIL after the event ring overflowed the exec callback ran {execs} times")
        return f"overflow:exec=1,{old}->{new}"
    finally:
        fx.close()


REQUEST_CONFIG = """(in-package #:tomoe-user)
(define-extension "e2e-request" (:reads (:windows :request) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (values state nil
          (when (and (member (getf event :type) '(:metadata :request)) (eq (getf event :request) :close))
            (list (spawn "echo close >> {closes}")))))
"""


def wl_string(text):
    raw = text.encode() + b"\0"
    return struct.pack("<I", len(raw)) + raw + b"\0" * (-len(raw) % 4)


def wl_messages(sock, until, timeout=5.0):
    buf, out = b"", []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r, _, _ = select.select([sock], [], [], 0.1)
        if not r:
            continue
        data = sock.recv(1 << 16)
        if not data:
            break
        buf += data
        while len(buf) >= 8:
            obj, word = struct.unpack_from("<II", buf)
            size = word >> 16
            if size < 8 or len(buf) < size:
                break
            out.append((obj, word & 0xFFFF, buf[8:size]))
            buf = buf[size:]
            if obj == until and (word & 0xFFFF) == 0:
                return out
    raise SystemExit("policy-effects: FAIL no Wayland sync reply from the foreign-toplevel client")


def foreign_close(sock_name, timeout=10.0):
    conn = connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], sock_name))
    try:
        conn.sendall(struct.pack("<III", 1, (12 << 16) | 1, 2) + sync_msg(3))
        name = None
        for obj, opcode, body in wl_messages(conn, 3):
            if obj == 2 and opcode == 0:
                length = struct.unpack_from("<I", body, 4)[0]
                if body[8:8 + length - 1] == b"zwlr_foreign_toplevel_manager_v1":
                    name = struct.unpack_from("<I", body, 0)[0]
        if name is None:
            raise SystemExit("policy-effects: FAIL no zwlr_foreign_toplevel_manager_v1 global")
        bind = struct.pack("<I", name) + wl_string("zwlr_foreign_toplevel_manager_v1") + struct.pack("<II", 1, 4)
        conn.sendall(struct.pack("<II", 2, ((8 + len(bind)) << 16) | 0) + bind)
        handles, serial = [], 5
        deadline = time.monotonic() + timeout
        while not handles and time.monotonic() < deadline:
            conn.sendall(sync_msg(serial))
            handles = [struct.unpack_from("<I", body)[0] for obj, opcode, body in wl_messages(conn, serial)
                if obj == 4 and opcode == 0]
            serial += 1
            if not handles:
                time.sleep(0.1)
        if len(handles) != 1:
            raise SystemExit(f"policy-effects: FAIL expected one foreign toplevel, have {len(handles)}")
        conn.sendall(struct.pack("<II", handles[0], (8 << 16) | 5) + sync_msg(serial))
        wl_messages(conn, serial)
    finally:
        conn.close()


def effects_request(tomoe_bin, client_bin, tag, flags):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    closes = os.path.join(runtime, f"e2e-request-{tag}.closes")
    if os.path.exists(closes):
        os.unlink(closes)
    config = os.path.join(runtime, f"e2e-request-{tag}.lisp")
    with open(config, "w") as f:
        f.write(REQUEST_CONFIG.format(closes=closes))
    sock = f"e2e-request-{tag}"
    tomoe = start_tomoe(tomoe_bin, sock, config, flags)
    app = None
    try:
        app = client(client_bin, sock, tag, 1)
        foreign_close(sock)
        deadline = time.monotonic() + 15
        while policy_generation(tomoe_bin, sock) < 2 and time.monotonic() < deadline:
            time.sleep(0.05)
        generation = policy_generation(tomoe_bin, sock)
        if generation < 2:
            raise SystemExit("policy-effects: FAIL the policy never crashed handling the close request")
        inspect(tomoe_bin, sock, 20)
        wait_lines(closes, 1, 5)
        time.sleep(1.0)
        count = len(lines_of(closes))
        if count != 1:
            raise SystemExit(f"policy-effects: FAIL a close request handled across a policy crash ran its effect {count} times")
        return f"request-{tag}:effect=1,generation={generation}"
    finally:
        if app:
            app.kill()
            app.wait()
        tomoe.terminate()
        tomoe.wait(timeout=10)


def effects_coalesce(tomoe_bin, extra=()):
    fx = Effects(tomoe_bin, "coalesce", ["--test-policy-boot-delay=1500", *extra])
    try:
        inspect(tomoe_bin, fx.sock, 20)
        wait_lines(fx.files["tick"], 2, 10)
        old = fx.policy()
        os.kill(old, signal.SIGKILL)
        new = fx.policy(old)
        before = len(lines_of(fx.files["tick"]))
        inspect(tomoe_bin, fx.sock, 20)
        time.sleep(0.3)
        burst = len(lines_of(fx.files["tick"])) - before
        if burst > 4:
            raise SystemExit(f"policy-effects: FAIL a 200 ms interval fired {burst} times across a 1.5 s policy boot")
        return f"coalesce:ticks={burst},{old}->{new}"
    finally:
        fx.close()


def effects_fresh(tomoe_bin, client_bin, extra=()):
    fx = Effects(tomoe_bin, "fresh", ["--test-policy-boot-delay=1500", *extra])
    chords = os.path.join(os.environ["XDG_RUNTIME_DIR"], "e2e-fx-fresh.chords")
    keys = None
    try:
        inspect(tomoe_bin, fx.sock, 20)
        keys = client(client_bin, fx.sock, "fx", 1, chords)
        read_until(keys, r"^phase", 10)
        if wait_lines(fx.files["launch"], 1, 10) != 1:
            raise SystemExit("policy-effects: FAIL the one-shot launch never ran")
        old = fx.policy()
        os.kill(old, signal.SIGKILL)
        new = fx.policy(old)
        with open(chords + ".tmp", "w") as f:
            f.write(f"{KEY_SUPER}+{KEY_N}\n")
        os.rename(chords + ".tmp", chords)
        read_until(keys, r"chords done", 5)
        inspect(tomoe_bin, fx.sock, 20)
        wait_lines(fx.files["fresh"], 1, 5)
        time.sleep(1.0)
        fresh = len(lines_of(fx.files["fresh"]))
        if fresh != 1:
            raise SystemExit(f"policy-effects: FAIL a new one-shot after resume ran {fresh} times")
        return f"fresh:launches=1,{old}->{new}"
    finally:
        if keys and keys.poll() is None:
            keys.kill()
            keys.wait()
        fx.close()


def effects_random(tomoe_bin, extra=()):
    seed = ["--test-policy-seed=4242", *extra]
    whole = Effects(tomoe_bin, "rng1", seed)
    try:
        inspect(tomoe_bin, whole.sock, 20)
        for i in range(6):
            whole.command("e2e-random")
            wait_lines(whole.files["draws"], i + 1, 5)
        expected = lines_of(whole.files["draws"])
    finally:
        whole.close()
    split = Effects(tomoe_bin, "rng2", seed)
    try:
        inspect(tomoe_bin, split.sock, 20)
        for i in range(3):
            split.command("e2e-random")
            wait_lines(split.files["draws"], i + 1, 5)
        time.sleep(0.3)
        kills = split.kill()
        for i in range(3, 6):
            split.command("e2e-random")
            wait_lines(split.files["draws"], i + 1, 5)
        got = lines_of(split.files["draws"])
    finally:
        split.close()
    if len(expected) != 6 or got != expected or len(set(got)) != 6:
        raise SystemExit(f"policy-effects: FAIL random draws across a kill {got} != uninterrupted {expected}")
    return f"rng:continues({','.join(got)}),{kills}"


def policy_idle_cli(tomoe_bin, config):
    sock = "e2e-idle"
    tomoe = start_tomoe(tomoe_bin, sock, config)
    try:
        inspect(tomoe_bin, sock, 20)
        time.sleep(1.5)
        old = policy_pid(tomoe)
        os.kill(old, signal.SIGSTOP)
        asked = time.monotonic()
        reply = subprocess.run([tomoe_bin, "--socket", sock, "inspect"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=30)
        answered = time.monotonic() - asked
        if reply.returncode != 0 or not reply.stdout.startswith("(2 :OK"):
            raise SystemExit(f"policy-stop: FAIL a default CLI request to a stopped idle policy failed "
                f"after {answered:.1f}s: {reply.stderr.strip()[:120]}")
        new = policy_pid(tomoe, old, 10)
        deadline = time.monotonic() + 5
        while os.path.exists(f"/proc/{old}") and time.monotonic() < deadline:
            time.sleep(0.05)
        if os.path.exists(f"/proc/{old}"):
            raise SystemExit(f"policy-stop: FAIL stopped idle policy {old} was never reaped")
        inspect(tomoe_bin, sock, 20)
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], f"tomoe.{sock}.sock"))
        conn.settimeout(25)
        conn.sendall(b'{"id":1,"method":"commands"}\n')
        first = b""
        while not first.endswith(b"\n"):
            chunk = conn.recv(65536)
            if not chunk:
                break
            first += chunk
        if b'"id"' not in first:
            raise SystemExit(f"policy-stop: FAIL the JSON connection never answered: {first[:80]!r}")
        time.sleep(4.0)
        held = policy_pid(tomoe)
        os.kill(held, signal.SIGSTOP)
        asked = time.monotonic()
        conn.sendall(b'{"id":2,"method":"commands"}\n')
        closed = None
        try:
            while closed is None:
                if not conn.recv(65536):
                    closed = time.monotonic() - asked
        except (socket.timeout, ConnectionResetError):
            closed = None if time.monotonic() - asked >= 25 else time.monotonic() - asked
        conn.close()
        if closed is None:
            raise SystemExit("policy-stop: FAIL a request on an open JSON connection to a stopped policy was never noticed")
        third = policy_pid(tomoe, held, 10)
        inspect(tomoe_bin, sock, 20)
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], f"tomoe.{sock}.sock"))
        conn.settimeout(10)
        conn.sendall(b'{"id":1,"method":"commands"}\n')
        first = b""
        while not first.endswith(b"\n"):
            chunk = conn.recv(65536)
            if not chunk:
                break
            first += chunk
        os.kill(third, signal.SIGSTOP)
        conn.sendall(b'{"id":3,"method":"commands"}\n')
        time.sleep(0.2)
        os.kill(third, signal.SIGKILL)
        rest = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                rest += chunk
        except ConnectionResetError:
            pass
        except socket.timeout:
            raise SystemExit("policy-stop: FAIL a JSON request in flight when the policy died left the connection open")
        conn.close()
        if b'"id":3' in rest:
            raise SystemExit(f"policy-stop: FAIL a JSON request in flight when the policy died was answered: {rest[:80]!r}")
        fourth = policy_pid(tomoe, third, 10)
        inspect(tomoe_bin, sock, 20)
        return f"idle-cli {old}->{new}:answered-in-{answered:.1f}s open-json {held}->{third}:closed-in-{closed:.1f}s " \
            f"killed-mid-request {third}->{fourth}:closed-unanswered"
    finally:
        tomoe.terminate()
        tomoe.wait(timeout=10)


def check_policy_stop(tomoe_bin, client_bin):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    config = os.path.join(runtime, "e2e-stop.lisp")
    with open(config, "w") as f:
        f.write("(in-package #:tomoe-user)\n(load-policy \"desktop\")\n")
    sock = "e2e-stop"
    tomoe = start_tomoe(tomoe_bin, sock, config, ["--test-policy-deadline-ms=1000"])
    frames = None
    try:
        frames = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "6",
            "--windows", "2"], env=dict(os.environ, WAYLAND_DISPLAY=sock), stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        time.sleep(1.0)
        old = policy_pid(tomoe)
        os.kill(old, signal.SIGSTOP)
        stopped = time.monotonic()
        poke = subprocess.Popen([client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "1",
            "--windows", "1", "--tag", "poke"], env=dict(os.environ, WAYLAND_DISPLAY=sock),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        new = policy_pid(tomoe, old, 10)
        poke.wait(timeout=20)
        took = time.monotonic() - stopped
        out, _ = frames.communicate(timeout=30)
        count = int(re.search(r"frames (\d+)", out).group(1))
        lost = int(re.search(r"lost (\d+)", out).group(1))
        deadline = time.monotonic() + 5
        while os.path.exists(f"/proc/{old}") and time.monotonic() < deadline:
            time.sleep(0.05)
        if os.path.exists(f"/proc/{old}"):
            raise SystemExit(f"policy-stop: FAIL stopped policy {old} was never reaped")
        if lost or count < 6 * 50:
            raise SystemExit(f"policy-stop: FAIL clients stalled while the policy was stopped: frames {count} lost {lost}")
        return f"policy-stop {old}->{new}:{took:.1f}s frames={count} lost=0 reaped " + \
            policy_idle_cli(tomoe_bin, config)
    finally:
        if frames and frames.poll() is None:
            frames.kill()
            frames.wait()
        tomoe.terminate()
        tomoe.wait(timeout=10)


KEY_SHIFT, KEY_TAB = 42 - 8, 15 - 8


def snapshot_toggle(tomoe_bin, client_bin, workspaces, tag, flags):
    runtime = os.environ["XDG_RUNTIME_DIR"]
    config = os.path.join(runtime, f"e2e-snap-{tag}.lisp")
    with open(workspaces) as f:
        text = f.read()
    with open(config, "w") as f:
        f.write(text)
    sock = f"e2e-snap-{tag}"
    chords = os.path.join(runtime, f"e2e-snap-{tag}.chords")
    tomoe = start_tomoe(tomoe_bin, sock, config, flags)
    clients = []
    try:
        clients.append(client(client_bin, sock, "a", 1))
        wait_windows(tomoe_bin, sock, 1)
        command(tomoe_bin, sock, "switch-tag", "tag=3")
        keys = client(client_bin, sock, "b", 1, chords)
        clients.append(keys)
        wait_windows(tomoe_bin, sock, 2)
        time.sleep(0.5)
        before = sorted(a for a, g in scene(inspect(tomoe_bin, sock))["layout"].items() if g[4])
        if before != ["bench-b-0"]:
            raise SystemExit(f"policy-snapshot: FAIL setup did not reach tag 3: {before}")
        first = policy_pid(tomoe)
        with open(chords + ".tmp", "w") as f:
            f.write(f"{KEY_SUPER}+{KEY_SHIFT}+{KEY_TAB}\n")
        os.rename(chords + ".tmp", chords)
        read_until(keys, r"chords done", 5)
        second = policy_pid(tomoe, first, 10)
        inspect(tomoe_bin, sock, 20)
        time.sleep(0.8)
        visible = sorted(a for a, g in scene(inspect(tomoe_bin, sock))["layout"].items() if g[4])
        return visible, f"{first}->{second}"
    finally:
        for c in clients:
            if c.poll() is None:
                c.kill()
                c.wait()
        tomoe.terminate()
        tomoe.wait(timeout=10)


def snapshot_case(tomoe_bin, client_bin, workspaces, tag, flags, what):
    visible, kills = snapshot_toggle(tomoe_bin, client_bin, workspaces, tag, flags)
    if visible != ["bench-a-0"]:
        raise SystemExit(f"policy-snapshot: FAIL {what}: toggled tag ended on {visible}, not tag 1")
    return f"{tag}:tag1,{kills}"


def check_policy_snapshot(tomoe_bin, client_bin, workspaces):
    return "policy-snapshot " + " ".join([
        snapshot_case(tomoe_bin, client_bin, workspaces, "before-done", ["--test-policy-crash-after-snapshot"],
            "kill after the snapshot delta, before DONE (double apply)"),
        snapshot_case(tomoe_bin, client_bin, workspaces, "after-done", ["--test-policy-crash-after-done"],
            "kill after DONE (state change lost)"),
        expect_failure("unstaged", lambda: snapshot_case(tomoe_bin, client_bin, workspaces, "unstaged",
            ["--test-policy-crash-after-snapshot", "--test-policy-snapshot-unstaged"], "unstaged")),
        expect_failure("late", lambda: snapshot_case(tomoe_bin, client_bin, workspaces, "late",
            ["--test-policy-crash-after-done", "--test-policy-snapshot-late"], "late")),
    ])


def check_policy_effects(tomoe_bin, client_bin):
    return "policy-effects " + " ".join([effects_duplicate(tomoe_bin), effects_lost(tomoe_bin),
        effects_pending(tomoe_bin), effects_downtime(tomoe_bin),
        "small-ring-" + effects_downtime(tomoe_bin, ["--test-policy-ring=16"]),
        effects_overflow(tomoe_bin), effects_coalesce(tomoe_bin),
        effects_request(tomoe_bin, client_bin, "lost", ["--test-policy-crash-before-flush"]),
        effects_request(tomoe_bin, client_bin, "dup", ["--test-policy-crash-before-done"]),
        effects_fresh(tomoe_bin, client_bin), effects_random(tomoe_bin)])


def expect_failure(name, run):
    try:
        run()
    except SystemExit as failure:
        return f"{name}-fails({str(failure).split('FAIL ', 1)[-1][:70]})"
    raise SystemExit(f"policy-effects: FAIL the {name} check passes on a build with that bug")


def check_policy_effects_bugs(tomoe_bin, client_bin):
    return "policy-effects-bugs " + " ".join([
        expect_failure("no-dedup", lambda: effects_duplicate(tomoe_bin, ["--test-policy-no-dedup"])),
        expect_failure("no-replay", lambda: effects_lost(tomoe_bin, ["--test-policy-no-replay"])),
        expect_failure("drop-pending", lambda: effects_pending(tomoe_bin, ["--test-policy-drop-pending"])),
        expect_failure("downtime-drop", lambda: effects_downtime(tomoe_bin, ["--test-policy-downtime-drop"])),
        expect_failure("timer-pileup", lambda: effects_coalesce(tomoe_bin, ["--test-policy-timer-pileup"])),
        expect_failure("kind-ids", lambda: effects_fresh(tomoe_bin, client_bin, ["--test-policy-sticky-dedup"])),
        expect_failure("rng-reset", lambda: effects_random(tomoe_bin, ["--test-policy-rng-reset"])),
    ])

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
        elif check == "policy-kill":
            results.append(check_policy_kill(args[3], client_bin, args[4]))
        elif check == "policy-stop":
            results.append(check_policy_stop(args[3], client_bin))
        elif check == "policy-snapshot":
            results.append(check_policy_snapshot(args[3], client_bin, args[4]))
        elif check == "policy-effects":
            results.append(check_policy_effects(args[3], client_bin))
        elif check == "policy-effects-bugs":
            results.append(check_policy_effects_bugs(args[3], client_bin))
        else:
            raise SystemExit(f"unknown check {check}")
    print("e2e-fault-isolation: " + " ".join(results))


main()
