# Top-level Makefile for the p100vram-shelf project.
#
# Dispatches into per-stage subdirs. The original nbd-vram (Stage 2
# baseline) keeps building exactly as before via the `baseline` target.

.PHONY: all baseline alloc gdr sidecar-cuda sidecar test test-integration kmod ci clean distclean help

UNAME_S := $(shell uname -s)

# Each subdir declares its own Makefile. If a subdir is absent (because
# its PR hasn't landed yet), skip it instead of erroring - this keeps
# the foundation buildable on its own.
define subdir
	@if [ -d "$(1)" ] && [ -f "$(1)/Makefile" ]; then \
		$(MAKE) -C "$(1)" $(2); \
	else \
		echo "[skip] $(1) (not present in this branch)"; \
	fi
endef

# Things that build on the Mac (the dev box). Order matters: the daemon
# links against the gdr library and the kernels object.
# The baseline nbd-vram is Linux-only (uses <endian.h>, mlockall, etc.)
# so it is excluded from `all` on non-Linux; build it via `make baseline`
# on the host.
ifeq ($(UNAME_S),Linux)
all: baseline gdr sidecar-cuda alloc sidecar
else
all: gdr sidecar-cuda sidecar
endif

# --- Stage 2 baseline (nbd-vram, source unchanged from upstream) ---
# The upstream build rule, kept inline so the baseline still builds with
# `make baseline` exactly as before: gcc + dlopen, no CUDA toolkit.
nbd-vram: nbd-vram.c
	gcc -O2 -Wall -o nbd-vram nbd-vram.c -ldl -lpthread

baseline: nbd-vram

# --- Stage 7: userspace allocator daemon ---
alloc:
	$(call subdir,kernel/allocator)

# --- Stage 7: kernel module (Linux only) ---
# Only swallow the "absent dir / wrong OS" case; a real build failure
# from an existing kernel/module dir must propagate, otherwise make kmod
# can report success without producing p100vram.ko.
kmod:
	@if [ ! -d kernel/module ] || [ ! -f kernel/module/Makefile ]; then \
		echo "[kmod] kernel/module absent - skipped"; \
	elif [ "$(UNAME_S)" != "Linux" ]; then \
		echo "[kmod] requires Linux kernel headers - skipped on $(UNAME_S)"; \
	else \
		$(MAKE) -C kernel/module modules; \
	fi

# --- Stage 8: GDRCopy write-path library ---
gdr:
	$(call subdir,gdr)

# --- Stage 9: CUDA kernels + per-GPU sidecar daemon ---
sidecar-cuda:
	$(call subdir,sidecar/cuda)

sidecar: gdr sidecar-cuda
	$(call subdir,sidecar/daemon)

# --- Tests ---
test:
	$(call subdir,test/unit,test)

test-integration:
	@if [ "$(UNAME_S)" = "Linux" ]; then \
		test/integration/run-on-host.sh; \
	else \
		echo "[test-integration] requires Linux + NVIDIA P100 (skipped on $(UNAME_S))"; \
		exit 1; \
	fi

# --- CI helper ---
ci: all test
	@echo "CI green on $(UNAME_S)"

clean:
	-$(call subdir,kernel/allocator,clean)
	-$(call subdir,kernel/module,clean)
	-$(call subdir,gdr,clean)
	-$(call subdir,sidecar/cuda,clean)
	-$(call subdir,sidecar/daemon,clean)
	-$(call subdir,test/unit,clean)

distclean: clean
	rm -f nbd-vram

help:
	@echo "Targets:"
	@echo "  all              - gdr + sidecar-cuda + sidecar (Mac);"
	@echo "                     + baseline + alloc on Linux"
	@echo "  baseline         - the unmodified nbd-vram daemon (Linux only)"
	@echo "  alloc            - Stage 7 userspace allocator (p100vram-alloc, Linux)"
	@echo "  kmod             - Stage 7 kernel module (Linux + kernel headers)"
	@echo "  gdr              - Stage 8 GDRCopy write-path library"
	@echo "  sidecar-cuda     - Stage 9 CUDA kernels (real or stub)"
	@echo "  sidecar          - Stage 9 per-GPU RPC daemon"
	@echo "  test             - unit tests (runnable on Mac)"
	@echo "  test-integration - host-only swap/pressure tests (Linux + P100)"
	@echo "  ci               - all + test"
	@echo "  clean / distclean"
