#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
system=$(nix eval --raw --impure --expr builtins.currentSystem)

tomoe_bin=${TOMOE_BIN:-$(nix build --no-link --print-out-paths "${root}#tomoe")/bin/tomoe}
client_bin=${CLIENT_BIN:-$(nix build --no-link --print-out-paths --impure --expr "let fl = builtins.getFlake \"path:${root}\"; pkgs = import fl.inputs.nixpkgs { system = \"${system}\"; }; in pkgs.callPackage (fl.outPath + \"/bench/client.nix\") { }")/bin/bench-client}
capture_bin=${CAPTURE_BIN:-$(nix build --no-link --print-out-paths --impure --expr "let fl = builtins.getFlake \"path:${root}\"; pkgs = import fl.inputs.nixpkgs { system = \"${system}\"; }; in pkgs.callPackage (fl.outPath + \"/bench/capture.nix\") { }")/bin/tomoe-e2e-capture}
layer_blur_bin=${LAYER_BLUR_BIN:-$(nix build --no-link --print-out-paths --impure --expr "let fl = builtins.getFlake \"path:${root}\"; pkgs = import fl.inputs.nixpkgs { system = \"${system}\"; }; in pkgs.callPackage (fl.outPath + \"/bench/e2e-layer-blur.nix\") { }")/bin/tomoe-e2e-layer-blur}

base=${XDG_RUNTIME_DIR:-/tmp}
dir=$(mktemp -d "${base}/tomoe-e2e.XXXXXX")
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/cap"

env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u TOMOE_SOCKET \
	"$root/tools/isolate" --check

env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u TOMOE_SOCKET \
	"$root/tools/isolate" "$dir" \
	bash -euo pipefail -c '
		tomoe_bin=$1
		client_bin=$2
		capture_bin=$3
		layer_blur_bin=$4
		cap_dir=$5
		log=$6

		"$tomoe_bin" --socket e2e --backend headless --bare --no-watch >"$log" 2>&1 &
		tpid=$!
		cleanup() {
			if kill -0 "$tpid" 2>/dev/null; then
				kill -TERM "$tpid" 2>/dev/null || true
				wait "$tpid" 2>/dev/null || true
			fi
		}
		trap cleanup EXIT

		ready=0
		for _ in $(seq 1 200); do
			if "$tomoe_bin" --socket e2e inspect >/dev/null 2>&1; then
				ready=1
				break
			fi
			kill -0 "$tpid" 2>/dev/null || { cat "$log" >&2; exit 1; }
			sleep 0.05
		done
		((ready == 1)) || { echo "e2e: tomoe not ready" >&2; cat "$log" >&2; exit 1; }

		[[ -S "$XDG_RUNTIME_DIR/e2e" ]] || { echo "e2e: no wayland socket" >&2; ls -la "$XDG_RUNTIME_DIR" >&2; exit 1; }
		wl_display="e2e"

		frames_out=$(WAYLAND_DISPLAY="$wl_display" "$client_bin" --input virtual --phase frames --seconds 1 --windows 1)
		echo "$frames_out"
		echo "$frames_out" | grep -Eq "frames [1-9][0-9]*"

		WAYLAND_DISPLAY="$wl_display" "$capture_bin" --output HEADLESS-1 --dump-dir "$cap_dir" --dump 1 >"$cap_dir/capture.log" 2>&1 &
		cpid=$!
		WAYLAND_DISPLAY="$wl_display" "$client_bin" --input virtual --phase frames --seconds 1 --windows 1 >/dev/null
		for _ in $(seq 1 50); do
			[[ -s "$cap_dir/frame-000001.ppm" ]] && break
			sleep 0.1
		done
		kill "$cpid" 2>/dev/null || true
		wait "$cpid" 2>/dev/null || true
		[[ -s "$cap_dir/frame-000001.ppm" ]] || { echo "e2e: capture failed" >&2; cat "$cap_dir/capture.log" >&2; exit 1; }

		blur_out=$(WAYLAND_DISPLAY="$wl_display" "$layer_blur_bin" --frames 3)
		echo "$blur_out"
		echo "$blur_out" | grep -Eq "frames=[3-9]" 

		python3 - "$tpid" "$XDG_RUNTIME_DIR/$wl_display" "$wl_display" "$client_bin" "$tomoe_bin" <<'PYEOF'
import os, re, signal, socket, struct, subprocess, sys, time

tpid = int(sys.argv[1])
wl_path, wl_display, client_bin, tomoe_bin = sys.argv[2:6]

def find_xwm(ppid):
    res = []
    for e in os.listdir("/proc"):
        if not e.isdigit(): continue
        try:
            st = open(f"/proc/{e}/stat").read()
            comm = st[st.find("(")+1:st.rfind(")")]
            pp = int(st.rsplit(")", 1)[1].split()[1])
            if pp == ppid and comm == "tomoe-xwm":
                res.append(int(e))
        except OSError:
            pass
    return res

xwm = []
for _ in range(40):
    xwm = find_xwm(tpid)
    if xwm: break
    time.sleep(0.05)
assert len(xwm) == 1, f"expected 1 tomoe-xwm, got {xwm}"
old_xwm = xwm[0]

cenv = os.environ.copy()
cenv["WAYLAND_DISPLAY"] = wl_display
honest = subprocess.Popen(
    [client_bin, "--input", "virtual", "--phase", "frames", "--seconds", "1", "--windows", "1"],
    env=cenv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
)
time.sleep(0.2)
os.kill(old_xwm, signal.SIGKILL)
new_xwm = None
for _ in range(60):
    cur = find_xwm(tpid)
    if cur and cur[0] != old_xwm:
        new_xwm = cur[0]
        break
    time.sleep(0.05)
assert new_xwm is not None, "tomoe-xwm did not respawn after SIGKILL"
hout, _ = honest.communicate(timeout=10)
assert honest.returncode == 0 and re.search(r"frames [1-9][0-9]*", hout) and re.search(r"lost 0\b", hout), hout

flood = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
flood.connect(wl_path)
flood.settimeout(2.0)
disc = False
oid = 2
try:
    for _ in range(200):
        buf = bytearray()
        for _ in range(250):
            buf += struct.pack("<III", 1, (12 << 16) | 0, oid)
            oid = 2 if oid >= 65000 else oid + 1
        flood.sendall(buf)
        time.sleep(0.005)
except (BrokenPipeError, ConnectionResetError, OSError):
    disc = True
if not disc:
    flood.settimeout(0.5)
    try:
        while flood.recv(4096): pass
        disc = True
    except (ConnectionResetError, BrokenPipeError):
        disc = True
    except socket.timeout:
        disc = False
flood.close()
assert disc, "stalled flooding client was not disconnected"

bad = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
bad.connect(wl_path)
bad.settimeout(1.0)
bad.sendall(struct.pack("<III", 1, (12 << 16) | 0, 65536))
eof = False
try:
    while bad.recv(4096): pass
    eof = True
except (ConnectionResetError, BrokenPipeError):
    eof = True
bad.close()
assert eof, "MAX_OBJECTS violation was not disconnected"
print(f"e2e-fault-isolation: xwm {old_xwm}->{new_xwm} flood=disconnected max_obj=rejected")
PYEOF

		trap - EXIT
		kill -TERM "$tpid"
		wait "$tpid"
		rc=$?
		((rc == 0)) || { echo "e2e: tomoe exited $rc" >&2; cat "$log" >&2; exit 1; }

		for sock in "$XDG_RUNTIME_DIR"/e2e "$XDG_RUNTIME_DIR"/wayland-*; do
			if [[ -e $sock ]]; then
				echo "e2e: leftover socket $sock" >&2
				exit 1
			fi
		done
		echo "e2e: PASS"
	' _ "$tomoe_bin" "$client_bin" "$capture_bin" "$layer_blur_bin" "$dir/cap" "$dir/tomoe.log"
