CC ?= cc
AR ?= ar
CFLAGS ?= -O2
OUT ?= build
PKG_CONFIG ?= pkg-config
WAYLAND_XML ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-scanner)/wayland.xml
SOURCES = $(wildcard *.c) $(OUT)/wayland-protocol.c
OBJECTS = $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
FLAGS = -std=c11 -D_GNU_SOURCE -Wno-unused-parameter -Wall -Wextra -Werror -fPIC -Iinclude -Iscanner -I$(OUT)

all: $(OUT)/libtomoe-wayland.a $(OUT)/libtomoe-wayland.so

$(OUT)/libtomoe-wayland.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUT)/libtomoe-wayland.so: $(OBJECTS)
	$(CC) -shared -Wl,-z,defs $^ -o $@

$(OUT)/%.o: %.c $(wildcard *.h include/*.h scanner/*.h) $(OUT)/wayland-server-protocol.h $(OUT)/wayland-client-protocol.h | $(OUT)
	$(CC) $(CFLAGS) $(FLAGS) -c $< -o $@

$(OUT)/wayland-protocol.o: $(OUT)/wayland-protocol.c $(wildcard include/*.h scanner/*.h)
	$(CC) $(CFLAGS) $(FLAGS) -c $< -o $@

$(OUT)/wayland-server-protocol.h: $(WAYLAND_XML) | $(OUT)
	wayland-scanner -c server-header $< $@

$(OUT)/wayland-client-protocol.h: $(WAYLAND_XML) | $(OUT)
	wayland-scanner -c client-header $< $@

$(OUT)/wayland-protocol.c: $(WAYLAND_XML) | $(OUT)
	wayland-scanner private-code $< $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
