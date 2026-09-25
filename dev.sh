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
for name in wlr-screencopy-unstable-v1 wlr-gamma-control-unstable-v1 wlr-foreign-toplevel-management-unstable-v1; do
  wlr_xml="$(dirname "$xml")/$name.xml"
  if [ ! -f "build/$name-protocol.c" ] || [ "$wlr_xml" -nt "build/$name-protocol.c" ]; then
    wayland-scanner server-header "$wlr_xml" "build/$name-protocol.h"
    wayland-scanner private-code "$wlr_xml" "build/$name-protocol.c"
  fi
done
effect_xml="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-background-effect/ext-background-effect-v1.xml"
for name in ext-image-capture-source-v1 ext-image-copy-capture-v1 ext-foreign-toplevel-list-v1 tearing-control-v1; do
  capture_xml="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/${name%-v1}/$name.xml"
  if [ ! -f "build/$name-protocol.c" ] || [ "$capture_xml" -nt "build/$name-protocol.c" ]; then
    wayland-scanner server-header "$capture_xml" "build/$name-protocol.h"
    wayland-scanner private-code "$capture_xml" "build/$name-protocol.c"
  fi
done
if [ ! -f build/ext-background-effect-v1-protocol.c ] || [ "$effect_xml" -nt build/ext-background-effect-v1-protocol.c ]; then
  wayland-scanner server-header "$effect_xml" build/ext-background-effect-v1-protocol.h
  wayland-scanner private-code "$effect_xml" build/ext-background-effect-v1-protocol.c
fi
if [ ! -f "$TOMOE_BACKEND_LIB" ] || [ -n "$(find native -name '*.c' -newer "$TOMOE_BACKEND_LIB" -print -quit)" ]; then
  cc -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror -Wno-unused-parameter \
    -fPIC -shared -Ibuild -I"$(pkg-config --variable=includedir wayland-protocols)" \
    $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm) \
    native/*.c build/*-protocol.c -o "$TOMOE_BACKEND_LIB" \
    $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm) -lm
fi

export TOMOE_BUILTINS="${TOMOE_BUILTINS:-$PWD/builtins/desktop.lisp}"
exec sbcl --noinform --load dev.lisp -- "$@"
