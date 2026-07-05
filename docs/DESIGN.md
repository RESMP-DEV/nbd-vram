# Design: P100 HBM2 memory shelf

This document carries the rationale of the 12-stage plan forward into
the codebase, so each `TODO(stage N)` marker has a home and a gate.

## Thesis

Deprecated datacenter GPUs (Tesla P100/V100-class) can be repurposed as
**power-managed, programmable ECC HBM2 spill tiers** for AI-agent
workloads — exploiting the gap between DRAM pricing and obsolete
accelerator resale pricing. DDR is the hot tier; HBM2 is the warm/cold
swap tier; NVMe/Optane is a safety floor only. CUDA sidecars turn the
shelf from "swap" into "near-data compute."

## Tiers, in priority order

| Priority | Tier | Role |
|---|---|---|
| highest | DDR | hot agent state, working set |
| 100 | P100 HBM2 swap (this project) | warm/cold resident state |
| low | NVMe/Optane | safety floor only |

Linux stripes equal-priority swap devices round-robin, so four P100s
at priority 100 act as one striped HBM2 shelf without any custom logic.

## How the four pieces fit

```
                +-----------------------------------------+
   kernel       |  p100vram.ko (Stage 7)                  |
   block dev    |  blk-mq + nvidia_p2p_get_pages          |
                |  /dev/p100vram<name> -> mkswap -> swapon|
                +-----------------------------------------+
                            ^  pins pages
                            |
                +-----------------------------------------+
   userspace    |  p100vram-alloc (Stage 7)               |
   allocator    |  cuMemAlloc, mlockall, IOC_CREATE       |
                +-----------------------------------------+
                                          \
                +--------------------------+--------------+
   userspace    |  p100vram-sidecar (Stage 9)             |
   sidecar      |  per-GPU RPC: hash/compress/scan/dedup/ |
   (one/GPU)    |  prefetch, backed by sm_60 CUDA kernels |
                +-----------------------------------------+
                            ^
                            |  small writes (Stage 8)
                +-----------------------------------------+
   gdr library  |  libp100vramgdr (Stage 8)               |
                |  GDRCopy BAR mapping + hybrid policy    |
                +-----------------------------------------+
```

The kernel module stays *boring*: no CUDA, no fake NUMA, no malloc into
HBM2. The cleverness lives in the userspace sidecar.

## Stage gates (from the plan)

- **Stage 2 (baseline, nbd-vram):** more stable agents before p95/p99
  collapse, without high sustained `pswpin`, runaway `pgmajfault`, or
  bad memory PSI.
- **Stage 6 (decision point):** pick the simplest path that wins on
  real agent latency. If NBD/CUDA is already good enough, leave kernel
  work for later. If GDRCopy reads are poor but writes are good, plan
  a hybrid (this is what `hybrid_policy.c` defaults encode).
- **Stage 7 (kmod MVP):** survives `mkswap`, `swapon`, pressure
  (`stress-ng`), `swapoff`, daemon restart, and card reset.
- **Stage 8 (hybrid):** kernel BAR write + CUDA read beats pure NBD on
  at least one meaningful dimension.
- **Stage 9 (sidecars):** kernels reduce page-in traffic or agent
  latency. Low-risk kernels first (checksum, compression probe, entropy
  classifier, heat scoring, dedup hash); high-value kernels later
  (compress cold state, dedup runtime blobs, vector scan, rerank).

## Safety invariants (do not violate)

1. **Always honor the NVIDIA P2P free callback.** Forgetting it = live
   swap pages pointing at freed VRAM = MCE-kill of every process that
   had pages swapped, PID 1 included. (Same hazard `nbd-vram.c` warns
   about for its daemon teardown.)
2. **Allocator daemon must not be paged out.** `mlockall(MCL_CURRENT |
   MCL_FUTURE)` + `PR_SET_IO_FLUSHER`, same as the baseline.
3. **Skeleton must fail cleanly, never lie about durability.** Until
   the real copy path lands, `queue_rq` returns `BLK_STS_IOERR`.
4. **No CUDA from kernel space.** All compute is userspace.

## CUDA target

P100 is compute capability 6.0, so all kernels compile with
`nvcc -O3 -arch=sm_60 -Xptxas=-v`. The `sidecar/cuda/Makefile`
hard-codes this.

## Out of scope for the skeleton pass

Stages 1–6 (hardware/ops), 10–12 (rollout, failure testing, paper
artifact) are operational work documented here but not encoded. The
code targets only the three pure-software stages: 7, 8, 9.
