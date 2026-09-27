#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
export TOMOE_SHELL="${TOMOE_SHELL:-$(command -v sh)}"
export TOMOE_BUILTINS="${TOMOE_BUILTINS:-$PWD/builtins/desktop.lisp}"
SBCL_HOME="$(dirname "$(readlink -f "$(command -v sbcl)")")/../lib/sbcl"
export SBCL_HOME

for module in executions watches notifications mpris battery network tray; do
  if [ ! -f "build/$module.o" ] || [ "support/$module.c" -nt "build/$module.o" ] || [ "support/$module.h" -nt "build/$module.o" ]; then
    cc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libsystemd) -c "support/$module.c" -o "build/$module.o"
  fi
done

if [ ! -f build/tomoe-xwayland ] || [ support/xwayland.c -nt build/tomoe-xwayland ]; then
  cc -std=c11 -Wall -Wextra -Werror support/xwayland.c -o build/tomoe-xwayland
fi
export PATH="$PWD/build:$PATH"

if ! command -v wayland-scanner >/dev/null 2>&1; then
  echo "dev.sh: wayland-scanner is required to build the backend" >&2
  exit 1
fi
wlr="${WLR_PROTOCOLS_XML:-$(pkg-config --variable=pkgdatadir wlr-protocols)}/unstable"
wp="$(pkg-config --variable=pkgdatadir wayland-protocols)"
for xml in \
  "$wlr/wlr-layer-shell-unstable-v1.xml" \
  "$wlr/wlr-screencopy-unstable-v1.xml" \
  "$wlr/wlr-gamma-control-unstable-v1.xml" \
  "$wlr/wlr-output-power-management-unstable-v1.xml" \
  "$wlr/wlr-foreign-toplevel-management-unstable-v1.xml" \
  "$wlr/wlr-data-control-unstable-v1.xml" \
  "$wlr/wlr-virtual-pointer-unstable-v1.xml" \
  native/virtual-keyboard-unstable-v1.xml \
  native/server-decoration.xml \
  "$wp/stable/xdg-shell/xdg-shell.xml" \
  "$wp/stable/linux-dmabuf/linux-dmabuf-v1.xml" \
  "$wp/stable/viewporter/viewporter.xml" \
  "$wp/stable/presentation-time/presentation-time.xml" \
  "$wp/staging/fractional-scale/fractional-scale-v1.xml" \
  "$wp/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml" \
  "$wp/staging/ext-image-capture-source/ext-image-capture-source-v1.xml" \
  "$wp/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml" \
  "$wp/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml" \
  "$wp/staging/ext-idle-notify/ext-idle-notify-v1.xml" \
  "$wp/staging/ext-session-lock/ext-session-lock-v1.xml" \
  "$wp/staging/xdg-activation/xdg-activation-v1.xml" \
  "$wp/staging/tearing-control/tearing-control-v1.xml" \
  "$wp/staging/ext-background-effect/ext-background-effect-v1.xml" \
  "$wp/staging/ext-data-control/ext-data-control-v1.xml" \
  "$wp/staging/pointer-warp/pointer-warp-v1.xml" \
  "$wp/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" \
  "$wp/unstable/relative-pointer/relative-pointer-unstable-v1.xml" \
  "$wp/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml" \
  "$wp/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" \
  "$wp/unstable/xdg-output/xdg-output-unstable-v1.xml" \
  "$wp/unstable/primary-selection/primary-selection-unstable-v1.xml"; do
  name=$(basename "$xml" .xml)
  if [ ! -f "build/$name-protocol.c" ] || [ "$xml" -nt "build/$name-protocol.c" ]; then
    wayland-scanner server-header "$xml" "build/$name-protocol.h"
    wayland-scanner private-code "$xml" "build/$name-protocol.c"
    wayland-scanner client-header "$xml" "build/$name-client-protocol.h"
  fi
done
if [ ! -f build/tomoe-runtime ] || [ -n "$(find native build -newer build/tomoe-runtime \( -name '*.[ch]' -o -name '*.o' \) -print -quit)" ]; then
  cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild -Wl,--export-dynamic \
    $(pkg-config --cflags wayland-server xkbcommon pixman-1 pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client lcms2) \
    "$SBCL_HOME/sbcl.o" native/*.c build/*-protocol.c build/*.o -o build/tomoe-runtime \
    $(pkg-config --libs wayland-server xkbcommon pixman-1 pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client lcms2 libsystemd) \
    -lresvg -ldl -lpthread -lzstd -lm
fi

exec build/tomoe-runtime --core "$SBCL_HOME/sbcl.core" --noinform --load dev.lisp -- "$@"
