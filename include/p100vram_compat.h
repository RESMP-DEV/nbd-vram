/* p100vram_compat.h - portability shim for NVIDIA P2P + GDRCopy symbols
 *
 * Lets the tree compile on macOS / without NVIDIA headers by providing
 * stub declarations. On Linux, when the real headers are available
 * (CONFIG_NVIDIA_P2P / HAVE_GDRCOPY), they take over instead.
 *
 * Nothing here is *implemented* in the skeleton; on the P100 host the
 * real symbols come from the NVIDIA out-of-tree kernel module (P2P) and
 * libgdrapi (GDRCopy). See docs/VALIDATION.md.
 */
#ifndef P100VRAM_COMPAT_H
#define P100VRAM_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * NVIDIA P2P (kernel-side): nvidia_p2p_get_pages / nvidia_p2p_put_pages
 *
 * Required for Stage 7: the blk-mq kmod pins GPU VRAM pages so the kernel
 * can DMA into them. NVIDIA requires a free/invalidation callback; the
 * skeleton stubs the types so the module compiles without the NVIDIA
 * kernel headers, and the kmod #ifdefs the actual calls.
 * ---------------------------------------------------------------------- */

#ifndef CONFIG_NVIDIA_P2P
/* Skeleton declarations only - real ones come from
 * include/linux/nvidia_p2p.h on the P100 host. */
struct nvidia_p2p_page_table;
struct nvidia_p2p_dma_mapping;

enum p100vram_p2p_page_table_kind {
    P100VRAM_P2P_KIND_STUB = 0
};

struct p100vram_p2p_free_arg {
    void *token;
};

typedef void (*p100vram_p2p_free_callback)(void *data);
#endif /* CONFIG_NVIDIA_P2P */

/* -------------------------------------------------------------------------
 * GDRCopy (userspace): gdr_open / gdr_pin_buffer / gdr_map / gdr_unmap
 *
 * Required for Stage 8: low-overhead CPU writes into BAR-mapped GPU
 * memory. libgdrapi is provided by the gdrcopy project on Linux; on
 * macOS we stub the prototypes so daemons link for unit tests.
 * ---------------------------------------------------------------------- */

#ifndef HAVE_GDRCOPY
struct gdr;
typedef struct gdr gdr_t;
typedef struct gdr_mh_s { unsigned int h; } gdr_mh_t;
#endif

#ifdef __cplusplus
}
#endif

#endif /* P100VRAM_COMPAT_H */
