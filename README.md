# nbd-vram

Use your NVIDIA GPU's VRAM as swap space on Linux.

Built for laptops with soldered memory and no upgrade path. If you have an RTX card sitting there with 8GB of VRAM and you're getting swapped to SSD, this puts that VRAM to work.

Tested on: RTX 3070 Laptop (GA104M, 16 GB physical, 8 GB VRAM), driver 580.159.03, kernel 6.17, Pop!_OS. Allocated 7 GB for swap. End result including zram and SSD swap ~46 GB, tripled the addressable memory. Overflow order is: RAM fills, then VRAM absorbs spill (fast, PCIe), then zram compresses the rest (CPU), then SSD only if everything else is exhausted.

---

## How it works

A small daemon allocates VRAM via the CUDA driver API, then serves it as a block device using the NBD (Network Block Device) protocol over a Unix socket. The kernel's built-in `nbd` driver connects to it and exposes `/dev/nbdX`. From there it's a normal swap device.

Data path: kernel swap subsystem - /dev/nbdX - nbd kernel driver - Unix socket - nbd-vram daemon - cuMemcpyHtoD/DtoH - GPU VRAM.

No kernel module to write or maintain. No NVIDIA kernel symbols. Survives kernel and driver updates without rebuilding anything.

---

## Why not the NVIDIA P2P API?

The "obvious" approach is `nvidia_p2p_get_pages_persistent`, which pins VRAM pages in BAR1 so the CPU can access them directly via `ioremap_wc`. Every existing project that tried this route hits the same wall: the NVIDIA driver returns `EINVAL` on consumer GeForce GPUs. Both the persistent and non-persistent variants, both flag values. It's gated at the RM level for Quadro/datacenter SKUs only, regardless of driver version.

The other approach - directly `ioremap_wc` the BAR1 physical address without going through the P2P API - also doesn't work. The GPU's internal page tables only have ~16 MiB of BAR1 mapped (just the display framebuffer). Reads from the rest return zeros. `mkswap` appears to succeed, then `swapon` fails because the swap header isn't actually there.

The NBD approach sidesteps all of this. `cuMemcpyHtoD` and `cuMemcpyDtoH` work on any CUDA GPU without any special permissions.

---

## Requirements

- NVIDIA GPU with CUDA support (any consumer RTX/GTX card)
- NVIDIA driver with `libcuda.so.1` (no CUDA toolkit needed)
- Linux kernel 3.0+ (nbd module, built into most distros)
- `nbd-client` package
- `gcc`, `make`

---

## Install

```sh
git clone https://github.com/c0dejedi/nbd-vram
cd nbd-vram
sudo ./install.sh
sudo systemctl start vram-swap-nbd
```

Verify:

```sh
swapon --show
# NAME       TYPE      SIZE USED PRIO
# /dev/nbd0  partition   7G   0B 1500
```

The service is enabled on install, so it comes up automatically on every boot.

---

## Configuration

Edit `/etc/systemd/system/vram-swap-nbd.service`:

```ini
Environment=VRAM_SETUP_SIZE_MB=7168    # how much VRAM to use
Environment=VRAM_SWAP_PRIORITY=1500   # swap priority (higher = used first)
```

The daemon tries the requested size first and backs off in 512 MiB steps if the GPU is short on memory - so it will grab as much as it can even if the display compositor is already loaded. `VRAM_SETUP_SIZE_MB` is the ceiling, not a hard requirement.

After changing, run `sudo systemctl daemon-reload && sudo systemctl restart vram-swap-nbd`.

---

## Smoke test (without installing)

```sh
sudo bash test-nbd.sh
```

Allocates VRAM, connects the NBD device, does a 1 MiB write/readback check, activates swap, then prints teardown instructions. `install.sh` handles teardown automatically if a test instance is running.

To stress the full partition after the smoke test passes:

```sh
sudo bash test-fill.sh
```

Writes the entire VRAM partition with zeros, verifies a sample read back, then auto-restores swap on exit.

---

## Performance

Measured on RTX 3070 Laptop via `test-fill.sh` (7 GiB sequential write, 4M blocks):

- Sequential throughput: ~1.3 GB/s
- Latency is lower than NVMe since the path goes through PCIe to GPU rather than storage

For laptops already using zram, set VRAM swap at a higher priority so it absorbs overflow before hitting SSD.

---

## Uninstall

```sh
sudo systemctl disable --now vram-swap-nbd
sudo rm /usr/local/bin/nbd-vram
sudo rm /usr/local/bin/nbd-vram-connect.sh
sudo rm /usr/local/bin/nbd-vram-disconnect.sh
sudo rm /etc/systemd/system/vram-swap-nbd.service
sudo systemctl daemon-reload
```

---

## License

MIT - Sean Lobjoit (c0dejedi)
