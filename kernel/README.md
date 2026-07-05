# Stage 7: p100vram blk-mq block device

The plan calls for a *boring* kernel block device: one disk per pinned GPU
allocation, no fake NUMA, no kernel-space CUDA. The interesting work is
deferred to Stage 9 (CUDA sidecar) and Stage 8 (hybrid read/write policy).

## Layout

- `module/p100vram.c`     - the kmod (blk-mq + NVIDIA P2P skeleton)
- `module/Kbuild`         - kernel build descriptor
- `module/Makefile`       - convenience wrapper around `make -C /lib/modules/.../build`
- `allocator/p100vram-alloc.c` - userspace CUDA allocator daemon
- `allocator/Makefile`

## How it fits the MVP design (plan Stage 7)

1. `p100vram-alloc` allocates a CUDA buffer on GPU N (`cuMemAlloc`).
2. It opens `/dev/p100vram-control` and issues `P100VRAM_IOC_CREATE` with
   the GPU VA, size, index, and disk name.
3. The kmod calls `nvidia_p2p_get_pages(gpu_va, size, &page_table,
   free_callback, disk)` and registers `/dev/p100vram<name>`.
4. Block requests copy between bio pages and the mapped GPU DMA pages.
5. On NVIDIA invalidation, `p100vram_p2p_free_cb` fires: freeze the queue,
   fail I/O, transition to `DEAD`. Never silently continue.

## Invariants (do not violate)

- **No CUDA from kernel space.** All CUDA is userspace.
- **No fake NUMA.** This is a block device, not a memory node.
- **No `malloc()` into HBM2.** The allocator owns the device buffer; the
  kmod only maps pages NVIDIA pinned for us.
- **Always honor the free callback.** Forgetting it = live swap pages
  point at freed VRAM = MCE-kill of every process that had pages swapped,
  PID 1 included. (Same hazard nbd-vram warns about.)

## Build (P100 host only)

```sh
cd kernel/module
make                              # builds p100vram.ko
sudo modprobe nvidia              # NVIDIA driver must be loaded first
sudo insmod p100vram.ko
ls -l /dev/p100vram-control
```

To enable the real P2P path, edit `Kbuild` and uncomment
`-DCONFIG_NVIDIA_P2P`, and ensure `nvidia_p2p.h` is reachable (typically
via the NVIDIA driver source tree).

## Skeleton status

`TODO(stage 7)` markers flag everything that is stubbed:

- bio iteration + page-aligned P2P DMA copy in `queue_rq`
- `nvidia_p2p_dma_map_pages` acquisition in `create_disk`
- queue freeze + `nvidia_p2p_put_pages` in `p100vram_p2p_free_cb`
- `P100VRAM_IOC_DESTROY` lookup-by-name + graceful teardown
- block-device fops + dma_alignment/virt_boundary tuning

The skeleton compiles without NVIDIA headers (P2P gated behind
`#ifdef CONFIG_NVIDIA_P2P`), loads, registers the control misc device,
and fails all disk I/O with `BLK_STS_IOERR` so a misconfigured build
never lies about durability.
