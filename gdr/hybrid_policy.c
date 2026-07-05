/* hybrid_policy.c - implementation of the policy selector (Stage 8)
 *
 * Pure logic, no I/O. Defaults encode the plan's expectation: GDRCopy
 * may win on small writes; CUDA DMA likely wins on reads and on large
 * writes. The selection runs on every I/O so the kernel block device
 * and the sidecar share the same rule set. Defaults are placeholders
 * pending Stage 6 microbench results; tune via env before first call.
 */

#include "hybrid_policy.h"

static const struct p100vram_policy_tunables k_defaults = {
    /* Below ~4 KiB, a CPU write via the BAR mapping should beat a
     * CUDA memcpy launch + sync. Above, the launch amortizes. */
    .gdr_write_threshold_bytes = 4 * 1024,
    /* Reads from BAR are typically slow on P100; default off until
     * microbench shows otherwise. */
    .gdr_read_threshold_bytes  = 0,
    .gdr_read_enabled          = 0,
};

const struct p100vram_policy_tunables *p100vram_default_tunables(void) {
    return &k_defaults;
}

enum p100vram_io_policy p100vram_select_policy(
        enum p100vram_io_dir dir,
        size_t size,
        const struct p100vram_policy_tunables *t)
{
    if (!t) t = &k_defaults;

    if (dir == P100VRAM_IO_READ) {
        /* "below this may try GDR" -> strictly less than */
        if (t->gdr_read_enabled && size < t->gdr_read_threshold_bytes)
            return P100VRAM_POLICY_ALL_GDR;
        return P100VRAM_POLICY_ALL_CUDA;
    }

    /* WRITE: ">= this size -> CUDA write" -> below the threshold uses GDR */
    if (size < t->gdr_write_threshold_bytes)
        return P100VRAM_POLICY_HYBRID;   /* GDR write, CUDA read path */
    return P100VRAM_POLICY_ALL_CUDA;
}
