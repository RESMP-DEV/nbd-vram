# test/unit/Makefile - auto-discovers available test modules.
#
# Links whatever test_*.c are present plus their dependencies. PR1
# (foundation) ships test_rpc.c + test_ioctl.c plus a local copy of
# rpc_framing.c; PR3 adds test_policy.c (and gdr/hybrid_policy.c);
# PR4 redirects rpc_framing to sidecar/daemon/.

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I../../include -I../../gdr -I.

BIN  := unit_tests

# Discover test modules from their sources (not .o, which don't exist
# yet at parse time - the classic wildcard pitfall).
TEST_SRCS := $(wildcard test_*.c)
TEST_OBJS := $(TEST_SRCS:.c=.o)

# rpc_framing source: prefer sidecar/daemon (PR4), else local copy (PR1).
ifneq ($(wildcard ../../sidecar/daemon/rpc_framing.c),)
RPC_SRC := ../../sidecar/daemon/rpc_framing.c
else
RPC_SRC := rpc_framing.c
endif
RPC_OBJ := $(RPC_SRC:.c=.o)

# hybrid_policy.c only exists once PR3 (Stage 8) lands; test_policy.c
# is only present in the same PR, so this is belt-and-braces.
GDR_OBJ :=
ifneq ($(wildcard ../../gdr/hybrid_policy.c),)
GDR_OBJ := ../../gdr/hybrid_policy.o
endif

all: $(BIN)

$(BIN): main.o $(TEST_OBJS) $(RPC_OBJ) $(GDR_OBJ)
	$(CC) $(CFLAGS) -o $@ main.o $(TEST_OBJS) $(RPC_OBJ) $(GDR_OBJ)

main.o: main.c main.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# test_*.c compile locally with the test includes.
test_%.o: test_%.c main.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# rpc_framing.o and hybrid_policy.o may live outside this dir; the
# pattern rule picks up their full path from $(RPC_SRC)/$(GDR_OBJ).
%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test: $(BIN)
	./$(BIN)

clean:
	rm -f main.o $(TEST_OBJS) $(BIN) rpc_framing.o

.PHONY: all test clean
