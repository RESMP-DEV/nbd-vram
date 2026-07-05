# Validation matrix: Mac vs. P100 host

This project is **authored on macOS** and **built/tested on a Linux
host with Tesla P100(s)**. The skeleton is structured so the maximum
amount of code compiles and tests green on the Mac *before* any
hardware is involved. This matrix says exactly what runs where.

| Component | Compiles on Mac? | Runs/tests on Mac? | Needs Linux? | Needs P100 / NVIDIA? |
|---|---|---|---|---|
| `nbd-vram.c` (Stage 2 baseline) | ✗ (Linux-only syscalls) | ✗ | ✅ | ✅ (libcuda.so.1) |
| `kernel/allocator/p100vram-alloc.c` | ✗ (`mlockall`, `PR_SET_IO_FLUSHER`, ioctl) | ✗ | ✅ | ✅ |
| `kernel/module/p100vram.c` | ✗ (Linux kernel headers) | ✗ | ✅ | ✅ (`nvidia_p2p.h`) |
| `gdr/hybrid_policy.c` | ✅ | ✅ (unit tested) | — | — |
| `gdr/gdr_writepath.c` | ✅ (stubs) | ✅ (returns `-ENOSYS`) | ✅ for real path | ✅ (libgdrapi) |
| `sidecar/cuda/kernels.cu` | ✗ (nvcc + sm_60) | ✗ | ✅ | ✅ (P100) |
| `sidecar/cuda/kernels_stub.c` | ✅ | ✅ | — | — |
| `sidecar/daemon/p100vram-sidecar.c` | ✅ (links stub) | ✅ (binds socket, but needs CUDA at runtime) | ✅ for real run | ✅ |
| `sidecar/daemon/rpc_framing.c` | ✅ | ✅ (unit tested) | — | — |
| `test/unit/main.c` | ✅ | ✅ (the green CI signal) | — | — |
| `test/integration/run-on-host.sh` | ✗ | ✗ (exits with skip) | ✅ | ✅ |

## What `make` does on each platform

### macOS (`darwin`)
- `make all` builds: `libp100vramgdr.a`, `p100vram-sidecar` (against
  `kernels_stub.o`), `p100vram-alloc` will *not* link (Linux-only
  syscalls) — it is skipped.
- `make test` runs the unit suite; it is the always-green signal.
- `make test-integration` prints a skip message and exits non-zero.

### Linux + CUDA toolkit (no P100 yet)
- `make alloc` builds the allocator daemon.
- `make sidecar` builds the daemon against real `kernels.o` (nvcc sm_60).
- `make kmod` builds `p100vram.ko`, but only with `CONFIG_NVIDIA_P2P`
  disabled in the Kbuild — so the module loads and creates the control
  device but every disk I/O returns `BLK_STS_IOERR` (safe no-op).
- `make test` still passes.

### Linux + P100 + NVIDIA driver
- Edit `kernel/module/Kbuild`: uncomment `-DCONFIG_NVIDIA_P2P`.
- `make kmod`, `sudo insmod kernel/module/p100vram.ko`.
- Run `scripts/deploy.sh` after exporting `P100_HOST`/`P100_USER`.
- Run `make test-integration` for the Stage-7 survival gate.
