CC ?= cc
AR ?= ar
CFLAGS ?= -O2
OUT ?= build
XKB_CONFIG_ROOT ?= /etc/X11/xkb
SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
FLAGS = -std=c11 -D_GNU_SOURCE -Wno-unused-parameter -Wall -Wextra -Werror -fPIC -Iinclude -DTOMOE_XKB_CONFIG_ROOT='"$(XKB_CONFIG_ROOT)"'

all: $(OUT)/libtomoe-xkb.a $(OUT)/libtomoe-xkb.so

$(OUT)/libtomoe-xkb.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUT)/libtomoe-xkb.so: $(OBJECTS)
	$(CC) -shared -Wl,-z,defs $^ -o $@

$(OUT)/%.o: %.c $(wildcard *.h include/*.h) | $(OUT)
	$(CC) $(CFLAGS) $(FLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
