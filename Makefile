CC ?= cc
NM ?= nm
PKG_CONFIG ?= pkg-config
VERSION ?= dev
CORE_STAMP ?= $(VERSION)
OPT ?= -O2
LTO ?=
CPU ?=
WLR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols)/unstable
WP ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_XML ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-scanner)/wayland.xml

LIBS = xkbcommon pangocairo libinput glesv2 egl gbm libudev
PORTAL_LIBS = libpipewire-0.3 gbm
SUPPORT = bus executions watches notifications mpris backlight battery network tray system pipewire
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
HEADERS = build/wayland-server-protocol.h build/wayland-client-protocol.h $(NAMES:%=build/%-protocol.h) $(NAMES:%=build/%-client-protocol.h)
LISP_SOURCES = prelude api ui cursor dialogs rules ipc plan runtime livepatch desktop default-config
LISP_EMBED = $(wildcard lisp/*.lisp)
LISP_NATIVE = $(patsubst native/lisp/%.c,build/obj/lisp-%.o,$(wildcard native/lisp/*.c))
WAYLAND_CORE = build/obj/wayland-wire.o build/obj/wayland-protocol.o
WAYLAND_SERVER = $(WAYLAND_CORE) build/obj/wayland-loop.o build/obj/wayland-server.o build/obj/wayland-shm.o
WAYLAND_CLIENT = $(WAYLAND_CORE) build/obj/wayland-client.o
NATIVE = $(patsubst native/%.c,build/obj/%.o,$(wildcard native/*.c)) $(LISP_NATIVE) $(NAMES:%=build/obj/%-protocol.o) $(WAYLAND_SERVER) build/obj/wayland-client.o
CFLAGS_CODE = $(OPT) $(LTO) $(CPU)
CFLAGS_NATIVE = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -fpatchable-function-entry=5,0 -DTOMOE_CORE_STAMP='"$(CORE_STAMP)"' -DTOMOE_VERSION='"$(VERSION)"' -DTOMOE_PATH_PREFIX='"$(PATH_PREFIX)"' -DTOMOE_FONTCONFIG_FILE='"$(FONTCONFIG_FILE)"' -Ibuild -Inative -Inative/wayland $(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS_CORE = $(CFLAGS_NATIVE) -fPIC -DTOMOE_SHARED_CORE
CFLAGS_SUPPORT = $(CFLAGS_CODE) -std=c11 -Wall -Wextra -Werror -fpatchable-function-entry=5,0 -DTOMOE_CORE_STAMP='"$(CORE_STAMP)"' -DTOMOE_VERSION='"$(VERSION)"'
CFLAGS_PORTAL = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild -Isupport -Inative/wayland $(shell $(PKG_CONFIG) --cflags $(PORTAL_LIBS))
CFLAGS_LISP_UNIT = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Inative

PORTAL = $(patsubst native/portal/%.c,build/obj/portal-%.o,$(wildcard native/portal/*.c)) build/bus.o
PORTAL_PROTOCOLS = linux-dmabuf-v1 ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1

vpath %.xml $(sort $(dir $(PROTOCOLS)))

all: build/tomoe build/tomoe-portal

build/tomoe-portal: $(PORTAL) $(PORTAL_PROTOCOLS:%=build/obj/%-protocol.o) $(WAYLAND_CLIENT)
	$(CC) $(LTO) $^ -o $@ $(shell $(PKG_CONFIG) --libs $(PORTAL_LIBS))

build/obj/portal-%.o: native/portal/%.c $(wildcard native/portal/*.h native/wayland/*.h) support/bus.h $(HEADERS) | build/obj
	$(CC) $(CFLAGS_PORTAL) -c $< -o $@

CORE_SOURCES = $(wildcard native/*.c) $(wildcard native/lisp/*.c) $(wildcard native/wayland/*.c)
CORE_OBJECTS = $(patsubst native/%.c,build/core/obj/%.o,$(CORE_SOURCES)) \
	$(NAMES:%=build/core/obj/protocol-%-protocol.o) $(SUPPORT:%=build/core/obj/support-%.o) \
	build/core/obj/wayland-protocol.o

build/tomoe: $(NATIVE) $(SUPPORT:%=build/%.o) | build/entries
	$(CC) $(LTO) $(NATIVE) $(SUPPORT:%=build/%.o) -o $@ $(shell $(PKG_CONFIG) --libs $(LIBS)) \
		-lresvg -lm
	build/entries $@

build/tomoe-core.so: $(CORE_OBJECTS) | build/entries
	$(CC) $(LTO) -shared -Wl,-Bsymbolic $(CORE_OBJECTS) -o $@ \
		$(shell $(PKG_CONFIG) --libs $(LIBS)) -lresvg -lm
	build/entries $@

core: build/tomoe-core.so

build/entries: tools/entries.c | build
	$(CC) $(CFLAGS_CODE) -std=c11 -Wall -Wextra -Werror -o $@ $<

build/core/obj/support-%.o: support/%.c $(wildcard support/*.h) | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_SUPPORT) -fPIC -DTOMOE_SHARED_CORE -c $< -o $@

build/core/obj/wayland-protocol.o: build/wayland-protocol.c build/wayland-server-protocol.h build/wayland-client-protocol.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_CORE) -c $< -o $@

build/core/obj/protocol-%-protocol.o: build/%-protocol.c $(HEADERS) | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_CORE) -c $< -o $@

build/core/obj/%.o: native/%.c $(wildcard native/*.h native/lisp/*.h native/wayland/*.h) $(HEADERS) | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_CORE) -c $< -o $@

build/embed: tools/embed.c | build
	$(CC) $(CFLAGS_CODE) -o $@ $<

build/lisp-sources.h: build/embed $(LISP_EMBED) FORCE
	sh -c 'set --; for n in $(LISP_SOURCES); do if [ -f lisp/$$n.lisp ]; then set -- "$$@" "$$n" "lisp/$$n.lisp"; fi; done; exec build/embed build/lisp-sources.h "$$@"'

build/obj/main.o: build/lisp-sources.h
build/core/obj/main.o: build/lisp-sources.h
build/obj/livecore.o: build/main-functions.h
build/core/obj/livecore.o: build/main-functions.h

build/main-functions.h: build/obj/main.o | build
	{ echo 'static const char *const livecore_driver[] = {'; \
	  $(NM) --defined-only --format=posix build/obj/main.o | \
	    awk '$$2 == "T" || $$2 == "t" { printf "    \"%s\",\n", $$1 }'; \
	  echo '};'; } > $@

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

build/obj/%.o: native/%.c $(wildcard native/*.h native/wayland/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/event-ring: tests/event-ring.c native/event.c native/policy.c $(wildcard native/*.h) $(HEADERS) $(WAYLAND_SERVER) | build
	$(CC) $(CFLAGS_NATIVE) -Inative -ffunction-sections -fdata-sections \
		tests/event-ring.c native/event.c native/policy.c $(WAYLAND_SERVER) -Wl,--gc-sections -lm -o $@

build/obj/wayland-%.o: native/wayland/%.c $(wildcard native/wayland/*.h) build/wayland-server-protocol.h build/wayland-client-protocol.h | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/obj/lisp-%.o: native/lisp/%.c $(wildcard native/lisp/*.h) $(wildcard native/*.h native/wayland/*.h) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/lisp-unit: $(wildcard native/lisp/*.c) $(wildcard native/lisp/*.h) native/lisp.h \
		native/state.c native/state.h native/region.c native/region.h tests/lisp-unit.c | build
	$(CC) $(CFLAGS_LISP_UNIT) -o $@ $(wildcard native/lisp/*.c) native/state.c native/region.c \
		tests/lisp-unit.c -lm -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

UNIT_SOURCES = tomoe:tests/lisp-api-stub.lisp user:lisp/desktop.lisp \
	$(patsubst %,user:%,$(sort $(wildcard examples/*.lisp))) \
	tomoe:tests/lisp-internal-tests.lisp user:tests/lisp-tests.lisp

lisp-unit: build/lisp-unit
	build/lisp-unit $(UNIT_SOURCES)

.PHONY: lisp-unit

build/obj/%-protocol.o: build/%-protocol.c $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

build/%.o: support/%.c $(wildcard support/*.h) | build
	$(CC) $(CFLAGS_SUPPORT) -c $< -o $@

build/wayland-server-protocol.h: $(WAYLAND_XML) | build
	wayland-scanner -c server-header $< $@

build/wayland-client-protocol.h: $(WAYLAND_XML) | build
	wayland-scanner -c client-header $< $@

build/wayland-protocol.c: $(WAYLAND_XML) | build
	wayland-scanner private-code $< $@

build/%-client-protocol.h: %.xml | build
	wayland-scanner client-header $< $@

build/%-protocol.h: %.xml | build
	wayland-scanner server-header $< $@

build/%-protocol.c: %.xml | build
	wayland-scanner private-code $< $@

build build/obj:
	mkdir -p $@

.PHONY: FORCE core
FORCE:

.SECONDARY:
