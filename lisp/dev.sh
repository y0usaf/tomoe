#!/usr/bin/env bash
# Run the compositor from the working tree. No Nix build, no saved image.
#   nix develop ./lisp -c ./lisp/dev.sh --backend lisp      # the Lisp backend
#   nix develop ./lisp -c ./lisp/dev.sh --backend headless  # the wlroots backend
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
  export TOMOE_LISP_BACKEND="$PWD/build/libtomoe-backend.so"
  if [ ! -f "$TOMOE_LISP_BACKEND" ] || [ native/backend.c -nt "$TOMOE_LISP_BACKEND" ]; then
    cc -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror -Wno-unused-parameter \
      -fPIC -shared -I"$(pkg-config --variable=includedir wayland-protocols)" \
      $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1) \
      native/backend.c -o "$TOMOE_LISP_BACKEND" \
      $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1)
  fi
fi

export TOMOE_LISP_BUILTINS="${TOMOE_LISP_BUILTINS:-$PWD/builtins/desktop.lisp}"
exec sbcl --noinform --load dev.lisp -- "$@"
