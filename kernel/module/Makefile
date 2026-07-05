# Stage 7: kernel/module/Makefile
#
# Wrapper around the kernel's own build system. Requires Linux kernel
# headers + the running kernel build dir. Does NOT work on macOS.

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules

modules:
	@if [ "$$(uname -s)" != "Linux" ]; then \
		echo "[p100vram] kmod build requires Linux kernel headers; skipping on $$(_OS)$$(_UNAME)"; \
		echo "[p100vram] on the P100 host run: make -C $(KDIR) M=$$(pwd) modules"; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$$(pwd) modules

clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
