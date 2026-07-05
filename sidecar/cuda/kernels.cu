/* kernels.cu - Stage 9 CUDA sidecar kernels for the P100 (sm_60)
 *
 * Per the plan: "Make the kernel block device boring and reliable;
 * make userspace CUDA sidecars clever." This file holds the *clever*:
 * near-data compute ops on resident pages, so we move results instead
 * of pages.
 *
 * Each kernel is a skeleton: compiles under nvcc -arch=sm_60 with a
 * trivial body and a TODO(stage 9) marker. Host-side launch wrappers
 * give the RPC daemon something to call. The wrappers are written in
 * CUDA C++ and exposed with C linkage so the daemon (C) can link them.
 *
 * Build on the P100 host:
 *   nvcc -O3 -arch=sm_60 -Xptxas=-v -I../../include -c kernels.cu -o kernels.o
 */

#include <cstdint>
#include <cstdlib>

#include "p100vram_kernels.h"   /* C-linkage wrappers */

/* -------------------------------------------------------------------------
 * Kernel skeletons. Each one is grid-1D, block-256, trivial body. Real
 * logic lands in Stage 9 once the memory shelf is reliable.
 * ---------------------------------------------------------------------- */

__global__ void hash_pages_kernel(const uint64_t *in, uint64_t count, uint64_t *out) {
    /* TODO(stage 9): crc32c / xxhash over 4 KiB pages, one block per page. */
    (void)in; (void)count;
    if (threadIdx.x == 0 && blockIdx.x == 0) *out = 0;
    return;
}

__global__ void compress_pages_kernel(const uint64_t *in, uint64_t count,
                                      uint64_t *out, uint64_t *out_bytes) {
    /* TODO(stage 9): zstd/lz4 probe, write compacted bytes + length. */
    (void)in; (void)count; (void)out;
    if (threadIdx.x == 0 && blockIdx.x == 0) *out_bytes = 0;
    return;
}

__global__ void scan_vectors_kernel(const float *vecs, uint64_t n,
                                    const float *query, uint64_t dim,
                                    float *out_best, uint64_t *out_idx) {
    /* TODO(stage 9): cosine similarity, top-k reduction. */
    (void)vecs; (void)n; (void)query; (void)dim;
    if (threadIdx.x == 0 && blockIdx.x == 0) { *out_best = 0.f; *out_idx = 0; }
    return;
}

__global__ void dedup_region_kernel(const uint64_t *in, uint64_t count,
                                    uint64_t *out_hashes) {
    /* TODO(stage 9): per-page hash into a shared table for dedup. */
    (void)in; (void)count; (void)out_hashes;
    return;
}

__global__ void prefetch_region_kernel(uint64_t *dst, const uint64_t *src,
                                       uint64_t count) {
    /* TODO(stage 9): coalesced device-to-device prefetch hint. */
    (void)dst; (void)src; (void)count;
    return;
}

/* -------------------------------------------------------------------------
 * C-linkage launch wrappers. These are what the daemon calls; they
 * stub to "no-op, success" so the RPC layer can be unit-tested on the
 * Mac via the CPU stub target.
 * ---------------------------------------------------------------------- */

extern "C" {

int p100vram_kern_hash_pages(uint64_t gpu_va, uint64_t length,
                             uint32_t algo, uint64_t *digest) {
    /* TODO(stage 9): cuMemAlloc scratch, launch hash_pages_kernel, sync,
     * copy digest back, free scratch. */
    (void)gpu_va; (void)length; (void)algo;
    *digest = 0;
    return 0;
}

int p100vram_kern_compress_pages(uint64_t gpu_va, uint64_t length,
                                 uint64_t out_va, uint64_t out_cap,
                                 uint64_t *out_bytes) {
    (void)gpu_va; (void)length; (void)out_va; (void)out_cap;
    *out_bytes = 0;
    return 0;
}

int p100vram_kern_scan_vectors(uint64_t vecs_va, uint64_t n,
                               uint64_t query_va, uint64_t dim,
                               float *out_best, uint64_t *out_idx) {
    (void)vecs_va; (void)n; (void)query_va; (void)dim;
    *out_best = 0.f; *out_idx = 0;
    return 0;
}

int p100vram_kern_dedup_region(uint64_t gpu_va, uint64_t length,
                               uint64_t hashes_va, uint64_t hashes_cap) {
    (void)gpu_va; (void)length; (void)hashes_va; (void)hashes_cap;
    return 0;
}

int p100vram_kern_prefetch_region(uint64_t dst_va, uint64_t src_va,
                                  uint64_t length) {
    (void)dst_va; (void)src_va; (void)length;
    return 0;
}

} /* extern "C" */
