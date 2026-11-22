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
        else:
            raise SystemExit(f"unknown check {check}")
    print("e2e-fault-isolation: " + " ".join(results))


main()
