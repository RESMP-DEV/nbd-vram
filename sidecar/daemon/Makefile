# Stage 9: sidecar/daemon/Makefile
#
# Links the RPC daemon against the kernels object (real or stub). The
# GDR write-path library (Stage 8) is optional: the daemon does not
# reference any GDR symbols today, so when gdr/ is absent (PR4 stacked
# on PR1 alone) the link still succeeds.

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../include -I../cuda -I../../gdr
LDLIBS   += -lpthread

OBJS := p100vram-sidecar.o rpc_framing.o
BIN  := p100vram-sidecar

# Pick the kernels object: prefer the real one if it was built.
KERNELS_OBJ :=
KERNELS_REAL :=
ifneq ($(wildcard ../cuda/kernels.o),)
KERNELS_OBJ  := ../cuda/kernels.o
KERNELS_REAL := yes
else ifneq ($(wildcard ../cuda/kernels_stub.o),)
KERNELS_OBJ := ../cuda/kernels_stub.o
endif

# GDR library is optional (Stage 8). Only link when present.
GDR_LIB :=
ifneq ($(wildcard ../../gdr/libp100vramgdr.a),)
GDR_LIB := ../../gdr/libp100vramgdr.a
endif

# Real CUDA objects (__global__ kernels) carry cudart registration
# references, so the final link must go through nvcc (or add -lcudart
# + the CUDA linker flags). Use nvcc when linking against kernels.o;
# otherwise gcc is fine for the stub path.
ifeq ($(KERNELS_REAL),yes)
NVCC     ?= nvcc
LINKER   := $(NVCC)
LDLIBS   += -lcudart
else
LINKER   := $(CC)
endif

all: $(BIN)

$(BIN): $(OBJS) $(KERNELS_OBJ) $(GDR_LIB)
	$(LINKER) $(CFLAGS) -o $@ $(OBJS) $(KERNELS_OBJ) $(GDR_LIB) $(LDLIBS)

p100vram-sidecar.o: p100vram-sidecar.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

rpc_framing.o: rpc_framing.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
