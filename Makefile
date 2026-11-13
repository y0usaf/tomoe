CC ?= cc
PKG_CONFIG ?= pkg-config
VERSION ?= dev
SBCL_HOME ?= $(shell dirname "$$(readlink -f "$$(command -v sbcl)")")/../lib/sbcl
WLR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols)/unstable
WP ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

LIBS = wayland-server xkbcommon pixman-1 pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client lcms2 libpipewire-0.3
SUPPORT = executions watches notifications mpris backlight battery network tray system
PROTOCOLS = \
	$(WLR)/wlr-layer-shell-unstable-v1.xml \
	$(WLR)/wlr-screencopy-unstable-v1.xml \
	$(WLR)/wlr-gamma-control-unstable-v1.xml \
	$(WLR)/wlr-output-power-management-unstable-v1.xml \
	$(WLR)/wlr-output-management-unstable-v1.xml \
	$(WLR)/wlr-foreign-toplevel-management-unstable-v1.xml \
	$(WLR)/wlr-data-control-unstable-v1.xml \
	$(WLR)/wlr-virtual-pointer-unstable-v1.xml \
	native/virtual-keyboard-unstable-v1.xml \
	native/server-decoration.xml \
	native/input-method-unstable-v2.xml \
	$(WP)/unstable/text-input/text-input-unstable-v3.xml \
	$(WP)/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml \
	$(WP)/stable/xdg-shell/xdg-shell.xml \
	$(WP)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
	$(WP)/stable/viewporter/viewporter.xml \
	$(WP)/stable/presentation-time/presentation-time.xml \
	$(WP)/staging/fractional-scale/fractional-scale-v1.xml \
	$(WP)/staging/fifo/fifo-v1.xml \
	$(WP)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml \
	$(WP)/staging/alpha-modifier/alpha-modifier-v1.xml \
	$(WP)/staging/content-type/content-type-v1.xml \
	$(WP)/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml \
	$(WP)/staging/xdg-dialog/xdg-dialog-v1.xml \
	$(WP)/staging/xdg-system-bell/xdg-system-bell-v1.xml \
	$(WP)/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml \
	$(WP)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
	$(WP)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
	$(WP)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
	$(WP)/staging/ext-idle-notify/ext-idle-notify-v1.xml \
	$(WP)/staging/ext-session-lock/ext-session-lock-v1.xml \
	$(WP)/staging/xdg-activation/xdg-activation-v1.xml \
	$(WP)/staging/tearing-control/tearing-control-v1.xml \
	$(WP)/staging/ext-background-effect/ext-background-effect-v1.xml \
	$(WP)/staging/ext-data-control/ext-data-control-v1.xml \
	$(WP)/staging/pointer-warp/pointer-warp-v1.xml \
	$(WP)/staging/cursor-shape/cursor-shape-v1.xml \
	$(WP)/stable/tablet/tablet-v2.xml \
	$(WP)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml \
	$(WP)/staging/security-context/security-context-v1.xml \
	$(WP)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
	$(WP)/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
	$(WP)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml \
	$(WP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
	$(WP)/unstable/xdg-output/xdg-output-unstable-v1.xml \
	$(WP)/unstable/primary-selection/primary-selection-unstable-v1.xml \
	$(WP)/staging/xwayland-shell/xwayland-shell-v1.xml

NAMES = $(basename $(notdir $(PROTOCOLS)))
HEADERS = $(NAMES:%=build/%-protocol.h) $(NAMES:%=build/%-client-protocol.h)
NATIVE = $(patsubst native/%.c,build/obj/%.o,$(wildcard native/*.c)) $(NAMES:%=build/obj/%-protocol.o)
CFLAGS_NATIVE = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild $(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS_SUPPORT = -std=c11 -Wall -Wextra -Werror -DTOMOE_VERSION='"$(VERSION)"' $(shell $(PKG_CONFIG) --cflags libsystemd)

vpath %.xml $(sort $(dir $(PROTOCOLS)))

build/tomoe-runtime: $(NATIVE) $(SUPPORT:%=build/%.o)
	$(CC) -Wl,--export-dynamic $(SBCL_HOME)/sbcl.o $^ -o $@ \
		$(shell $(PKG_CONFIG) --libs $(LIBS) libsystemd) -lresvg -ldl -lpthread -lzstd -lm

build/test-client: tests/client.c $(HEADERS)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild tests/client.c $(NAMES:%=build/%-protocol.c) \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/pointer-path: tests/pointer-path.c build/wlr-virtual-pointer-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS_NATIVE) -Ibuild tests/pointer-path.c \
		build/wlr-virtual-pointer-unstable-v1-protocol.c \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/obj/%.o: native/%.c $(wildcard native/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/obj/%-protocol.o: build/%-protocol.c $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/%.o: support/%.c $(wildcard support/*.h) | build
	$(CC) $(CFLAGS_SUPPORT) -c $< -o $@

build/%-client-protocol.h: %.xml | build
	wayland-scanner client-header $< $@

build/%-protocol.h: %.xml | build
	wayland-scanner server-header $< $@

build/%-protocol.c: %.xml | build
	wayland-scanner private-code $< $@

build build/obj:
	mkdir -p $@

.SECONDARY:
