#!/usr/bin/env bash
# Run the compositor from the working tree. No Nix build, no saved image.
#   nix develop -c ./dev.sh --backend lisp      # the Lisp backend
#   nix develop -c ./dev.sh --backend headless  # the wlroots backend
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

args=("$@")
backend=nested
for i in "${!args[@]}"; do
  [ "${args[$i]}" = "--backend" ] && backend="${args[$((i + 1))]:-}"
done

if [ "$backend" = "lisp" ]; then
  : # the Lisp backend needs no compiled shim
else
  export TOMOE_BACKEND_LIB="$PWD/build/libtomoe-backend.so"
  protocol=build/wlr-layer-shell-unstable-v1-protocol.h
  xml="${WLR_PROTOCOLS_XML:-$(pkg-config --variable=pkgdatadir wlr-protocols 2>/dev/null || true)}/unstable/wlr-layer-shell-unstable-v1.xml"
  if ! command -v wayland-scanner >/dev/null 2>&1; then
    echo "dev.sh: wayland-scanner is required to build the wlroots backend" >&2
    exit 1
  fi
  if [ ! -f "$xml" ]; then
    echo "dev.sh: cannot find wlr-layer-shell-unstable-v1.xml; set WLR_PROTOCOLS_XML to the wlr-protocols share directory" >&2
    exit 1
  fi
  if [ ! -f "$protocol" ] || [ "$xml" -nt "$protocol" ]; then
    wayland-scanner server-header "$xml" "$protocol"
  fi
  if [ ! -f "$TOMOE_BACKEND_LIB" ] || [ native/backend.c -nt "$TOMOE_BACKEND_LIB" ]; then
    cc -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror -Wno-unused-parameter \
      -fPIC -shared -Ibuild -I"$(pkg-config --variable=includedir wayland-protocols)" \
      $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1 xcb xcb-ewmh xcb-icccm) \
      native/backend.c -o "$TOMOE_BACKEND_LIB" \
      $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1)
  fi
fi

export TOMOE_BUILTINS="${TOMOE_BUILTINS:-$PWD/builtins/desktop.lisp}"
exec sbcl --noinform --load dev.lisp -- "$@"
