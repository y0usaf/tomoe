#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
system=$(nix eval --raw --impure --expr builtins.currentSystem)

tomoe_bin=${TOMOE_BIN:-$(nix build --no-link --print-out-paths "${root}#tomoe")/bin/tomoe}
client_bin=${CLIENT_BIN:-$(nix build --no-link --print-out-paths --impure --expr "let fl = builtins.getFlake \"path:${root}\"; pkgs = import fl.inputs.nixpkgs { system = \"${system}\"; }; in pkgs.callPackage (fl.outPath + \"/bench/client.nix\") { }")/bin/bench-client}
capture_bin=${CAPTURE_BIN:-$(nix build --no-link --print-out-paths --impure --expr "let fl = builtins.getFlake \"path:${root}\"; pkgs = import fl.inputs.nixpkgs { system = \"${system}\"; }; in pkgs.callPackage (fl.outPath + \"/bench/capture.nix\") { }")/bin/tomoe-e2e-capture}

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
		cap_dir=$4
		log=$5

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
	' _ "$tomoe_bin" "$client_bin" "$capture_bin" "$dir/cap" "$dir/tomoe.log"
