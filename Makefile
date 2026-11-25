CC ?= cc
PKG_CONFIG ?= pkg-config
VERSION ?= dev
XKB_CONFIG_ROOT ?= /etc/X11/xkb
OPT ?= -O2
LTO ?=
CPU ?=
WLR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols)/unstable
WP ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_XML ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-scanner)/wayland.xml

LIBS = freetype2 libinput vulkan
LINK = $(shell $(PKG_CONFIG) --libs $(LIBS))
PORTAL_LIBS = vulkan
SUPPORT = executions watches notifications mpris backlight battery network system
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
LIBRARIES = png bus wayland xkb pipewire udev
LIB_SOURCES = $(foreach l,$(LIBRARIES),$(wildcard lib/$(l)/*.c))
LIB_HEADERS = $(wildcard lib/*/*.h lib/*/include/*.h lib/wayland/scanner/*.h)
LIB_INCLUDE = $(LIBRARIES:%=-Ilib/%/include) -Ilib/wayland/scanner
LIB_OBJECTS = $(patsubst lib/%.c,build/obj/lib/%.o,$(filter-out lib/wayland/%,$(LIB_SOURCES)))
WAYLAND_CORE = build/obj/lib/wayland/wire.o build/obj/wayland-protocol.o
WAYLAND_SERVER = $(WAYLAND_CORE) build/obj/lib/wayland/loop.o build/obj/lib/wayland/server.o build/obj/lib/wayland/shm.o
WAYLAND_CLIENT = $(WAYLAND_CORE) build/obj/lib/wayland/client.o
NATIVE = $(patsubst native/%.c,build/obj/%.o,$(wildcard native/*.c)) $(LIB_OBJECTS) $(NAMES:%=build/obj/%-protocol.o) $(WAYLAND_SERVER) build/obj/lib/wayland/client.o
CFLAGS_CODE = $(OPT) $(LTO) $(CPU)
CFLAGS_BASE = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS_NATIVE = $(CFLAGS_BASE) -DTOMOE_VERSION='"$(VERSION)"' -DTOMOE_PATH_PREFIX='"$(PATH_PREFIX)"' -Ibuild -Inative $(LIB_INCLUDE) $(shell $(PKG_CONFIG) --cflags $(LIBS))
CFLAGS_SUPPORT = $(CFLAGS_CODE) -std=c11 -Wall -Wextra -Werror -DTOMOE_VERSION='"$(VERSION)"' -Ilib/bus/include
CFLAGS_PORTAL = $(CFLAGS_CODE) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Ibuild -Ilib/bus/include -Ilib/wayland/include -Ilib/wayland/scanner -Ilib/pipewire/include $(shell $(PKG_CONFIG) --cflags $(PORTAL_LIBS))
LIB_CFLAGS_bus = $(CFLAGS_SUPPORT)
LIB_CFLAGS_pipewire = $(CFLAGS_SUPPORT)
LIB_FLAGS_wayland = -Ilib/wayland/scanner -Ibuild
LIB_FLAGS_xkb = -DTOMOE_XKB_CONFIG_ROOT='"$(XKB_CONFIG_ROOT)"'
lib_cflags = $(or $(LIB_CFLAGS_$(1)),$(CFLAGS_BASE)) -Ilib/$(1)/include $(LIB_FLAGS_$(1))

PORTAL = $(patsubst native/portal/%.c,build/obj/portal-%.o,$(wildcard native/portal/*.c)) build/obj/lib/bus/bus.o build/obj/lib/pipewire/pwwire.o
PORTAL_PROTOCOLS = linux-dmabuf-v1 ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1

vpath %.xml $(sort $(dir $(PROTOCOLS)))

XWM = $(patsubst native/xwm/%.c,build/obj/xwm-%.o,$(wildcard native/xwm/*.c))

LISP_OBJECTS = $(patsubst lib/%.c,build/obj/lib/%.o,$(wildcard lib/lisp/*.c))
POLICY = $(patsubst policy/%.c,build/obj/policy-%.o,$(wildcard policy/*.c)) $(LISP_OBJECTS) build/obj/pv.o build/obj/desktop-paths.o build/obj/lib/wayland/wire.o
GLSLANG ?= glslangValidator
CFLAGS_POLICY = $(CFLAGS_BASE) -DTOMOE_VERSION='"$(VERSION)"' -DTOMOE_GLSLANG='"$(GLSLANG)"' -Ipolicy -Inative -Ilib/lisp/include -Ilib/wayland/include -Ilib/wayland/scanner -Ibuild

all: build/tomoe build/tomoe-portal build/tomoe-xwm build/tomoe-policy

build/tomoe-policy: $(POLICY)
	$(CC) $(LTO) $^ -o $@ -lm

build/obj/policy-%.o: policy/%.c $(wildcard policy/*.h) native/pv.h native/policy_proto.h native/shader-vk.h native/desktop-session.h $(wildcard lib/lisp/include/*.h) | build/obj
	$(CC) $(CFLAGS_POLICY) -c $< -o $@

build/obj/policy-main.o: $(wildcard lisp/*.lisp)

build/tomoe-xwm: $(XWM)
	$(CC) $(LTO) $^ -o $@

build/obj/xwm-%.o: native/xwm/%.c native/xwm/xwm.h native/xwm_proto.h | build/obj
	$(CC) $(CFLAGS_BASE) -Inative -Inative/xwm -c $< -o $@

build/tomoe-portal: $(PORTAL) $(PORTAL_PROTOCOLS:%=build/obj/%-protocol.o) $(WAYLAND_CLIENT)
	$(CC) $(LTO) $^ -o $@ $(shell $(PKG_CONFIG) --libs $(PORTAL_LIBS))

build/obj/portal-%.o: native/portal/%.c $(wildcard native/portal/*.h) $(LIB_HEADERS) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_PORTAL) -c $< -o $@

build/tomoe: $(NATIVE) $(SUPPORT:%=build/%.o)
	$(CC) $(LTO) $(NATIVE) $(SUPPORT:%=build/%.o) -o $@ $(LINK) -lm \
		-Wl,--export-dynamic-symbol=tomoe_png_glyph_info \
		-Wl,--export-dynamic-symbol=tomoe_png_glyph_rows

build/obj/%.o: native/%.c $(wildcard native/*.h) $(LIB_HEADERS) $(HEADERS) | build/obj
	$(CC) $(CFLAGS_NATIVE) -c $< -o $@

SHADER_SOURCES = $(wildcard native/shaders/*.vert native/shaders/*.frag native/shaders/*.comp)
# Fragment shaders that discard: glslang turns discard into OpDemoteToHelperInvocation for
# Vulkan 1.3, which needs the shaderDemoteToHelperInvocation feature. The .kill builds target
# Vulkan 1.2 (OpKill) for a device that lacks it.
SHADER_KILL = scene.frag ext.frag
SHADER_HEADERS = $(patsubst native/shaders/%,build/shaders/%.h,$(SHADER_SOURCES)) \
	$(SHADER_KILL:%=build/shaders/%.kill.h)

build/obj/gfx_vk.o: $(SHADER_HEADERS)

build/shaders/%.kill.spv: native/shaders/% | build/shaders
	glslangValidator -V --target-env vulkan1.2 -o $@ $<

build/shaders/%.spv: native/shaders/% | build/shaders
	glslangValidator -V --target-env vulkan1.3 -o $@ $<

build/shaders/%.h: build/shaders/%.spv
	od -An -v -tx4 $< | awk -v name=$(subst .,_,$*) 'BEGIN { printf "static const uint32_t %s[] = {\n", name } { for (i = 1; i <= NF; i++) printf "0x%s,", $$i; printf "\n" } END { printf "};\n" }' > $@

build/obj/lib/%.o: lib/%.c $(LIB_HEADERS) build/wayland-server-protocol.h build/wayland-client-protocol.h | build/obj
	@mkdir -p $(dir $@)
	$(CC) $(call lib_cflags,$(firstword $(subst /, ,$*))) -c $< -o $@

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

build build/obj build/shaders:
	mkdir -p $@

.SECONDARY:
