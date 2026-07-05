# Stage 9: sidecar/daemon/Makefile
#
# Links the RPC daemon against the kernels object (real or stub) and
# against the GDR write-path library (Stage 8).

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../include -I../cuda -I../../gdr
LDLIBS   += -lpthread

OBJS := p100vram-sidecar.o rpc_framing.o
BIN  := p100vram-sidecar

# Pick the kernels object: prefer the real one if it was built.
KERNELS_OBJ :=
ifneq ($(wildcard ../cuda/kernels.o),)
KERNELS_OBJ := ../cuda/kernels.o
else ifneq ($(wildcard ../cuda/kernels_stub.o),)
KERNELS_OBJ := ../cuda/kernels_stub.o
endif

GDR_LIB := ../../gdr/libp100vramgdr.a

all: $(BIN)

$(BIN): $(OBJS) $(KERNELS_OBJ) $(GDR_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(KERNELS_OBJ) $(GDR_LIB) $(LDLIBS)

p100vram-sidecar.o: p100vram-sidecar.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

rpc_framing.o: rpc_framing.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
