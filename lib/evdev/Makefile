CC ?= cc
AR ?= ar
CFLAGS ?= -O2
OUT ?= build
SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
FLAGS = -std=c11 -D_GNU_SOURCE -Wno-unused-parameter -Wall -Wextra -Werror -fPIC -Iinclude/libevdev-1.0

all: $(OUT)/libtomoe-evdev.a $(OUT)/libtomoe-evdev.so

$(OUT)/libtomoe-evdev.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUT)/libtomoe-evdev.so: $(OBJECTS)
	$(CC) -shared -Wl,-z,defs $^ -o $@

$(OUT)/%.o: %.c $(wildcard *.h) $(wildcard include/libevdev-1.0/libevdev/*.h) | $(OUT)
	$(CC) $(CFLAGS) $(FLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
