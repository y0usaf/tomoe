#!/usr/bin/env bash
# Build the Wayland test client against protocol code generated at build time.
# Needs pkg-config, wayland-scanner and the wayland client library.
#   build-client.sh [OUTPUT]        default: <tests>/client
set -euo pipefail

tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output="${1:-$tests/client}"
scanner="${WAYLAND_SCANNER:-wayland-scanner}"

command -v "$scanner" >/dev/null 2>&1 || {
  echo "build-client: wayland-scanner is not on PATH" >&2
  exit 1
}
command -v pkg-config >/dev/null 2>&1 || {
  echo "build-client: pkg-config is not on PATH" >&2
  exit 1
}

wayland_protocols="$(pkg-config --variable=pkgdatadir wayland-protocols)"
wlr_protocols="${WLR_PROTOCOLS_XML:-$(pkg-config --variable=pkgdatadir wlr-protocols 2>/dev/null || true)}"
xdg_xml="$wayland_protocols/stable/xdg-shell/xdg-shell.xml"
wlr_xml="$wlr_protocols/unstable/wlr-layer-shell-unstable-v1.xml"

[ -f "$xdg_xml" ] || {
  echo "build-client: $xdg_xml is missing; install wayland-protocols" >&2
  exit 1
}
[ -f "$wlr_xml" ] || {
  echo "build-client: $wlr_xml is missing; install wlr-protocols or set WLR_PROTOCOLS_XML" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/tomoe-client-XXXXXX")"
trap 'rm -rf "$work"' EXIT

"$scanner" client-header "$xdg_xml" "$work/xdg-shell-client-protocol.h"
"$scanner" private-code "$xdg_xml" "$work/xdg-shell-protocol.c"
"$scanner" client-header "$wlr_xml" "$work/wlr-layer-shell-unstable-v1-client-protocol.h"
"$scanner" private-code "$wlr_xml" "$work/wlr-layer-shell-unstable-v1-protocol.c"

cflags="$(pkg-config --cflags wayland-client)"
libs="$(pkg-config --libs wayland-client)"
libdir="$(pkg-config --variable=libdir wayland-client)"
cc="${CC:-cc}"

# Generated code is not ours to police; client.c is compiled strictly.
# shellcheck disable=SC2086
"$cc" -std=c11 -O2 $cflags -I"$work" -c "$work/xdg-shell-protocol.c" -o "$work/xdg-shell.o"
# shellcheck disable=SC2086
"$cc" -std=c11 -O2 $cflags -I"$work" -c "$work/wlr-layer-shell-unstable-v1-protocol.c" \
  -o "$work/wlr-layer-shell.o"
# shellcheck disable=SC2086
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter $cflags -I"$work" \
  -c "$tests/client.c" -o "$work/client.o"

mkdir -p "$(dirname "$output")"
# shellcheck disable=SC2086
"$cc" "$work/client.o" "$work/xdg-shell.o" "$work/wlr-layer-shell.o" $libs \
  -Wl,-rpath,"$libdir" -o "$output"
echo "build-client: $output"
