CC ?= cc
AR ?= ar
CFLAGS ?= -O2
OUT ?= build
SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,$(OUT)/%.o,$(notdir $(SOURCES)))
FLAGS = -std=c11 -Wall -Wextra -Werror -fPIC -Iinclude

all: $(OUT)/libtomoe-pipewire.a $(OUT)/libtomoe-pipewire.so

$(OUT)/libtomoe-pipewire.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(OUT)/libtomoe-pipewire.so: $(OBJECTS)
	$(CC) -shared -Wl,-z,defs $^ -o $@

$(OUT)/%.o: %.c $(wildcard *.h include/*.h) | $(OUT)
	$(CC) $(CFLAGS) $(FLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
