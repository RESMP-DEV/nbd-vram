/* hybrid_policy.h - choose the read/write transport per I/O (Stage 8)
 *
 * The plan's expectation: CPU writes into mapped GPU memory may be
 * good; CPU reads may be bad. So a hybrid policy - BAR/GDRCopy on
 * writes, CUDA DMA on reads - may beat both pure paths.
 *
 * This header is the *selection* layer. The actual transport plumbing
 * lives in gdr_writepath.c (GDR/BAR) and the CUDA sidecar (DMA). The
 * selection function is pure logic and is unit-tested on the Mac; the
 * default returns ALL_CUDA (current nbd-vram behavior) until Stage 6
 * microbench data justifies switching.
 */
#ifndef P100VRAM_HYBRID_POLICY_H
#define P100VRAM_HYBRID_POLICY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum p100vram_io_dir {
    P100VRAM_IO_READ  = 0,    /* device -> host: DtoH                */
    P100VRAM_IO_WRITE = 1,    /* host -> device: HtoD                */
};

enum p100vram_io_policy {
    P100VRAM_POLICY_ALL_CUDA = 0,    /* cuMemcpy{HtoD,DtoH}Async    */
    P100VRAM_POLICY_ALL_GDR  = 1,    /* GDRCopy/BAR mapping both dir*/
    P100VRAM_POLICY_HYBRID   = 2,    /* GDR writes + CUDA DMA reads */
};

/* Tunables. Defaults are placeholders pending Stage 6 microbench. The
 * daemon may override them via env vars before the first call. */
struct p100vram_policy_tunables {
    size_t  gdr_write_threshold_bytes;  /* >= this size -> CUDA write */
    size_t  gdr_read_threshold_bytes;   /* reads below this may try GDR */
    int     gdr_read_enabled;           /* 0 = trust CUDA for reads   */
};

const struct p100vram_policy_tunables *p100vram_default_tunables(void);

enum p100vram_io_policy p100vram_select_policy(
        enum p100vram_io_dir dir,
        size_t size,
        const struct p100vram_policy_tunables *t);

#ifdef __cplusplus
}
#endif

#endif /* P100VRAM_HYBRID_POLICY_H */
