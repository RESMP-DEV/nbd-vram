# Stage 9: sidecar/cuda/Makefile
#
# Two targets:
#   kernels.o  - real CUDA kernels via nvcc (Linux + CUDA toolkit only)
#   kernels_stub.o - CPU-only stubs, for linking the daemon on macOS
#
# The P100 is sm_60; that is the target architecture per the plan.

NVCC     ?= nvcc
NVCCFLAGS = -O3 -arch=sm_60 -Xptxas=-v -std=c++14
CPPFLAGS += -I../../include -I.

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -fPIC

# Default: prefer the real kernels if nvcc is on PATH AND actually
# works (some hosts ship a shim that fails at runtime), else fall back
# to the stub so the daemon still links for unit tests.
# Override explicitly with WITH_CUDA=1 or WITH_CUDA=0.
NVCC_PATH    := $(shell command -v $(NVCC) 2>/dev/null)
NVCC_WORKS   := $(shell $(NVCC_PATH) --version >/dev/null 2>&1 && echo yes)

ifeq ($(WITH_CUDA),1)
TARGET := kernels.o
else ifeq ($(WITH_CUDA),0)
TARGET := kernels_stub.o
else ifneq ($(NVCC_WORKS),yes)
TARGET := kernels_stub.o
else
TARGET := kernels.o
endif

all: $(TARGET)
	@if [ "$(TARGET)" = "kernels_stub.o" ]; then \
		echo "[sidecar-cuda] using CPU stub (no working nvcc; set WITH_CUDA=1 on the P100 host)"; \
	fi

kernels.o: kernels.cu
	$(NVCC) $(NVCCFLAGS) $(CPPFLAGS) -c -o $@ $<

kernels_stub.o: kernels_stub.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f kernels.o kernels_stub.o

.PHONY: all clean
