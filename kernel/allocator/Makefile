# Stage 7: kernel/allocator/Makefile
# Mirrors the baseline nbd-vram build: no CUDA toolkit needed at build
# time, only libcuda.so.1 at runtime via dlopen.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../include
LDLIBS  += -ldl -lpthread

BIN     := p100vram-alloc

all: $(BIN)

$(BIN): p100vram-alloc.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
