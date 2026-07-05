/* p100vram_kernels.h - C-linkage launch wrappers for the Stage 9 kernels.
 *
 * Implemented in kernels.cu (CUDA C++, sm_60). A CPU-only stub
 * (kernels_stub.c) provides the same symbols for Mac unit-test builds.
 */
#ifndef P100VRAM_KERNELS_H
#define P100VRAM_KERNELS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int p100vram_kern_hash_pages    (uint64_t gpu_va, uint64_t length,
                                 uint32_t algo, uint64_t *digest);
int p100vram_kern_compress_pages(uint64_t gpu_va, uint64_t length,
                                 uint64_t out_va, uint64_t out_cap,
                                 uint64_t *out_bytes);
int p100vram_kern_scan_vectors  (uint64_t vecs_va, uint64_t n,
                                 uint64_t query_va, uint64_t dim,
                                 float *out_best, uint64_t *out_idx);
int p100vram_kern_dedup_region  (uint64_t gpu_va, uint64_t length,
                                 uint64_t hashes_va, uint64_t hashes_cap);
int p100vram_kern_prefetch_region(uint64_t dst_va, uint64_t src_va,
                                  uint64_t length);

#ifdef __cplusplus
}
#endif

#endif /* P100VRAM_KERNELS_H */
