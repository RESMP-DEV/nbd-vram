/* kernels_stub.c - CPU-only stubs of the Stage 9 kernel wrappers.
 *
 * Lets the sidecar daemon link and run its RPC unit tests on macOS,
 * where nvcc and a P100 are unavailable. Same symbols as kernels.cu;
 * never linked into a build that also has kernels.o.
 */

#include <stdint.h>
#include "p100vram_kernels.h"

int p100vram_kern_hash_pages    (uint64_t gpu_va, uint64_t length,
                                 uint32_t algo, uint64_t *digest) {
    (void)gpu_va; (void)length; (void)algo; *digest = 0; return 0;
}
int p100vram_kern_compress_pages(uint64_t gpu_va, uint64_t length,
                                 uint64_t out_va, uint64_t out_cap,
                                 uint64_t *out_bytes) {
    (void)gpu_va; (void)length; (void)out_va; (void)out_cap; *out_bytes = 0; return 0;
}
int p100vram_kern_scan_vectors  (uint64_t vecs_va, uint64_t n,
                                 uint64_t query_va, uint64_t dim,
                                 float *out_best, uint64_t *out_idx) {
    (void)vecs_va; (void)n; (void)query_va; (void)dim; *out_best = 0.f; *out_idx = 0; return 0;
}
int p100vram_kern_dedup_region  (uint64_t gpu_va, uint64_t length,
                                 uint64_t hashes_va, uint64_t hashes_cap) {
    (void)gpu_va; (void)length; (void)hashes_va; (void)hashes_cap; return 0;
}
int p100vram_kern_prefetch_region(uint64_t dst_va, uint64_t src_va,
                                  uint64_t length) {
    (void)dst_va; (void)src_va; (void)length; return 0;
}
