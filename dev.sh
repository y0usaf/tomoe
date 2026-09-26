#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
export TOMOE_EXEC_LIB="$PWD/build/libtomoe-executions.so"
export TOMOE_SHELL="${TOMOE_SHELL:-$(command -v sh)}"
if [ ! -f "$TOMOE_EXEC_LIB" ] || [ support/executions.c -nt "$TOMOE_EXEC_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared support/executions.c -o "$TOMOE_EXEC_LIB"
fi

export TOMOE_WATCH_LIB="$PWD/build/libtomoe-watches.so"
if [ ! -f "$TOMOE_WATCH_LIB" ] || [ support/watches.c -nt "$TOMOE_WATCH_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared support/watches.c -o "$TOMOE_WATCH_LIB"
fi

export TOMOE_NOTIFICATION_LIB="$PWD/build/libtomoe-notifications.so"
if [ ! -f "$TOMOE_NOTIFICATION_LIB" ] || [ support/notifications.c -nt "$TOMOE_NOTIFICATION_LIB" ] || [ support/notifications.h -nt "$TOMOE_NOTIFICATION_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    $(pkg-config --cflags libsystemd) support/notifications.c \
    -o "$TOMOE_NOTIFICATION_LIB" $(pkg-config --libs libsystemd)
fi

export TOMOE_MPRIS_LIB="$PWD/build/libtomoe-mpris.so"
if [ ! -f "$TOMOE_MPRIS_LIB" ] || [ support/mpris.c -nt "$TOMOE_MPRIS_LIB" ] || [ support/mpris.h -nt "$TOMOE_MPRIS_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    $(pkg-config --cflags libsystemd) support/mpris.c \
    -o "$TOMOE_MPRIS_LIB" $(pkg-config --libs libsystemd)
fi

export TOMOE_BATTERY_LIB="$PWD/build/libtomoe-battery.so"
if [ ! -f "$TOMOE_BATTERY_LIB" ] || [ support/battery.c -nt "$TOMOE_BATTERY_LIB" ] || [ support/battery.h -nt "$TOMOE_BATTERY_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    $(pkg-config --cflags libsystemd) support/battery.c \
    -o "$TOMOE_BATTERY_LIB" $(pkg-config --libs libsystemd)
fi

export TOMOE_NETWORK_LIB="$PWD/build/libtomoe-network.so"
if [ ! -f "$TOMOE_NETWORK_LIB" ] || [ support/network.c -nt "$TOMOE_NETWORK_LIB" ] || [ support/network.h -nt "$TOMOE_NETWORK_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    $(pkg-config --cflags libsystemd) support/network.c \
    -o "$TOMOE_NETWORK_LIB" $(pkg-config --libs libsystemd)
fi

export TOMOE_TRAY_LIB="$PWD/build/libtomoe-tray.so"
if [ ! -f "$TOMOE_TRAY_LIB" ] || [ support/tray.c -nt "$TOMOE_TRAY_LIB" ] || [ support/tray.h -nt "$TOMOE_TRAY_LIB" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
    $(pkg-config --cflags libsystemd) support/tray.c \
    -o "$TOMOE_TRAY_LIB" $(pkg-config --libs libsystemd)
fi

if [ ! -f build/tomoe-xwayland ] || [ support/xwayland.c -nt build/tomoe-xwayland ]; then
  cc -std=c11 -Wall -Wextra -Werror support/xwayland.c -o build/tomoe-xwayland
fi
export PATH="$PWD/build:$PATH"

export TOMOE_BACKEND_LIB="$PWD/build/libtomoe-backend.so"
if ! command -v wayland-scanner >/dev/null 2>&1; then
  echo "dev.sh: wayland-scanner is required to build the backend" >&2
  exit 1
fi
wlr="${WLR_PROTOCOLS_XML:-$(pkg-config --variable=pkgdatadir wlr-protocols)}/unstable"
wp="$(pkg-config --variable=pkgdatadir wayland-protocols)"
kde="${PLASMA_WAYLAND_PROTOCOLS_XML:?set PLASMA_WAYLAND_PROTOCOLS_XML to the plasma-wayland-protocols share directory}"
for xml in \
  "$wlr/wlr-layer-shell-unstable-v1.xml" \
  "$wlr/wlr-screencopy-unstable-v1.xml" \
  "$wlr/wlr-gamma-control-unstable-v1.xml" \
  "$wlr/wlr-foreign-toplevel-management-unstable-v1.xml" \
  "$wlr/wlr-data-control-unstable-v1.xml" \
  "$wlr/wlr-virtual-pointer-unstable-v1.xml" \
  native/virtual-keyboard-unstable-v1.xml \
  "$kde/server-decoration.xml" \
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
if [ ! -f "$TOMOE_BACKEND_LIB" ] || [ -n "$(find native -name '*.c' -newer "$TOMOE_BACKEND_LIB" -print -quit)" ]; then
  cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter \
    -fPIC -shared -Ibuild -I"$(pkg-config --variable=includedir wayland-protocols)" \
    $(pkg-config --cflags wayland-server xkbcommon pixman-1 pangocairo libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm libseat libudev wayland-client) \
    native/*.c build/*-protocol.c -o "$TOMOE_BACKEND_LIB" \
    $(pkg-config --libs wayland-server xkbcommon pixman-1 pangocairo libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm libseat libudev wayland-client) -lm
fi

export TOMOE_BUILTINS="${TOMOE_BUILTINS:-$PWD/builtins/desktop.lisp}"
exec sbcl --noinform --load dev.lisp -- "$@"
