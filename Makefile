CC ?= cc
PKG_CONFIG ?= pkg-config
VERSION ?= dev
OPT ?= -O2
LTO ?=
CPU ?=
WLR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols)/unstable
WP ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

LIBS = wayland-server xkbcommon pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client libpipewire-0.3
PORTAL_LIBS = wayland-client libpipewire-0.3 libsystemd libdrm gbm
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
LISP_SOURCES = prelude api runtime rules dialogs ui cursor ipc desktop-lib default-config
LISP_EMBED = $(wildcard lisp/*.lisp) builtins/desktop.lisp
LISP_NATIVE = $(patsubst native/lisp/%.c,build/obj/lisp-%.o,$(wildcard native/lisp/*.c))
NATIVE = $(patsubst native/%.c,build/obj/%.o,$(wildcard native/*.c)) $(LISP_NATIVE) $(NAMES:%=build/obj/%-protocol.o)
CFLAGS_CODE = $(OPT) $(LTO) $(CPU)
CFLAGS_NATIVE = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -DTOMOE_VERSION='"$(VERSION)"' -DTOMOE_PATH_PREFIX='"$(PATH_PREFIX)"' -DTOMOE_FONTCONFIG_FILE='"$(FONTCONFIG_FILE)"' -Ibuild -Inative $(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS_SUPPORT = $(CFLAGS_CODE) -std=c11 -Wall -Wextra -Werror -DTOMOE_VERSION='"$(VERSION)"' $(shell $(PKG_CONFIG) --cflags libsystemd)
CFLAGS_PORTAL = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild $(shell $(PKG_CONFIG) --cflags $(PORTAL_LIBS))
CFLAGS_LISP_UNIT = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Inative

PORTAL = $(patsubst native/portal/%.c,build/obj/portal-%.o,$(wildcard native/portal/*.c))
PORTAL_PROTOCOLS = linux-dmabuf-v1 ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1

vpath %.xml $(sort $(dir $(PROTOCOLS)))

all: build/tomoe build/tomoe-portal

build/tomoe-portal: $(PORTAL) $(PORTAL_PROTOCOLS:%=build/obj/%-protocol.o)
	$(CC) $(LTO) $^ -o $@ $(shell $(PKG_CONFIG) --libs $(PORTAL_LIBS))

build/obj/portal-%.o: native/portal/%.c $(wildcard native/portal/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_PORTAL) -c $< -o $@

build/tomoe: $(NATIVE) $(SUPPORT:%=build/%.o)
	$(CC) $(LTO) $^ -o $@ $(shell $(PKG_CONFIG) --libs $(LIBS) libsystemd) \
		-lresvg -ldl -lpthread -lzstd -lm

build/embed: tools/embed.c | build
	$(CC) $(CFLAGS_CODE) -o $@ $<

build/lisp-sources.h: build/embed $(LISP_EMBED) FORCE
	sh -c 'set --; for n in $(LISP_SOURCES); do if [ -f lisp/$$n.lisp ]; then set -- "$$@" "$$n" "lisp/$$n.lisp"; fi; done; set -- "$$@" desktop builtins/desktop.lisp; exec build/embed build/lisp-sources.h "$$@"'

build/obj/main.o: build/lisp-sources.h

build/test-client: tests/client.c $(HEADERS)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild tests/client.c $(NAMES:%=build/%-protocol.c) \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/pointer-path: tests/pointer-path.c build/wlr-virtual-pointer-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS_NATIVE) -Ibuild tests/pointer-path.c \
		build/wlr-virtual-pointer-unstable-v1-protocol.c \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/pointer-script: tests/pointer-script.c build/wlr-virtual-pointer-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS_NATIVE) -Ibuild tests/pointer-script.c \
		build/wlr-virtual-pointer-unstable-v1-protocol.c \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/dmabuf-client: tests/dmabuf-client.c build/linux-dmabuf-v1-client-protocol.h build/xdg-shell-client-protocol.h build/viewporter-client-protocol.h
	$(CC) $(CFLAGS_NATIVE) -Ibuild tests/dmabuf-client.c \
		build/linux-dmabuf-v1-protocol.c build/xdg-shell-protocol.c build/viewporter-protocol.c \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client gbm libdrm) -o $@

build/warp-client: tests/warp-client.c tests/client.c $(HEADERS)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild -Itests tests/warp-client.c \
		$(NAMES:%=build/%-protocol.c) \
		$(shell $(PKG_CONFIG) --cflags --libs wayland-client) -o $@

build/obj/%.o: native/%.c $(wildcard native/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/event-ring: tests/event-ring.c native/event.c native/policy.c $(wildcard native/*.h) $(HEADERS) | build
	$(CC) $(CFLAGS_NATIVE) -Inative -ffunction-sections -fdata-sections \
		tests/event-ring.c native/event.c native/policy.c -Wl,--gc-sections \
		$(shell $(PKG_CONFIG) --libs wayland-server) -lm -o $@

build/obj/lisp-%.o: native/lisp/%.c $(wildcard native/lisp/*.h) $(wildcard native/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/lisp-unit: $(wildcard native/lisp/*.c) $(wildcard native/lisp/*.h) native/lisp.h tests/lisp-unit.c | build
	$(CC) $(CFLAGS_LISP_UNIT) -o $@ $(wildcard native/lisp/*.c) tests/lisp-unit.c -lm \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

UNIT_SOURCES = tomoe:tests/lisp-api-stub.lisp user:builtins/desktop.lisp \
	$(patsubst %,user:%,$(sort $(wildcard examples/*.lisp))) \
	tomoe:tests/lisp-internal-tests.lisp user:tests/lisp-tests.lisp

lisp-unit: build/lisp-unit
	build/lisp-unit $(UNIT_SOURCES)

.PHONY: lisp-unit

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

.PHONY: FORCE
FORCE:

.SECONDARY:
