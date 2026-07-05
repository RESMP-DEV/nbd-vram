# Stage 8: gdr/Makefile
#
# Builds libp100vramgdr.a: the GDRCopy write-path wrapper + the hybrid
# policy selector. The wrapper TU compiles against stubs on macOS; on
# Linux with libgdrapi present, link the final daemon against -lgdrapi
# to override the stubs.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
CPPFLAGS += -I../include -I.

OBJS_LIB := hybrid_policy.o gdr_writepath.o
LIB      := libp100vramgdr.a

# Optional real GDRCopy. pkg-config gdrcopy is the conventional name.
GDR_PC := $(shell pkg-config --exists gdrcopy 2>/dev/null && echo yes)
ifeq ($(GDR_PC),yes)
CFLAGS  += $(shell pkg-config --cflags gdrcopy)
GDR_LIB := $(shell pkg-config --libs   gdrcopy)
else
GDR_LIB :=
endif

all: $(LIB)

$(LIB): $(OBJS_LIB)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS_LIB) $(LIB)

.PHONY: all clean
