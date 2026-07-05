/* test/unit/test_policy.c - hybrid policy selection tests (PR3 Stage 8). */
#include "hybrid_policy.h"
#include "main.h"

static int test_policy_defaults(void) {
    const struct p100vram_policy_tunables *t = p100vram_default_tunables();
    /* Read always CUDA by default */
    if (p100vram_select_policy(P100VRAM_IO_READ, 100, t) != P100VRAM_POLICY_ALL_CUDA) return 1;
    /* Small write -> hybrid (GDR write) */
    if (p100vram_select_policy(P100VRAM_IO_WRITE, 64, t) != P100VRAM_POLICY_HYBRID) return 2;
    /* Large write -> CUDA */
    if (p100vram_select_policy(P100VRAM_IO_WRITE, 64 * 1024, t) != P100VRAM_POLICY_ALL_CUDA) return 3;
    /* NULL tunables -> defaults apply */
    if (p100vram_select_policy(P100VRAM_IO_WRITE, 64, NULL) != P100VRAM_POLICY_HYBRID) return 4;
    return 0;
}

static int test_policy_gdr_read_disabled_by_default(void) {
    const struct p100vram_policy_tunables *t = p100vram_default_tunables();
    /* Even a tiny read should be CUDA since gdr_read_enabled=0 */
    return p100vram_select_policy(P100VRAM_IO_READ, 1, t) != P100VRAM_POLICY_ALL_CUDA;
}

/* Threshold is documented as ">= this size -> CUDA". An exact-threshold
 * write must go CUDA, not GDR; this locks the off-by-one fix in place. */
static int test_policy_write_threshold_is_inclusive_cuda(void) {
    const struct p100vram_policy_tunables *t = p100vram_default_tunables();
    if (p100vram_select_policy(P100VRAM_IO_WRITE, t->gdr_write_threshold_bytes, t)
        != P100VRAM_POLICY_ALL_CUDA) return 1;
    /* One byte below the threshold still uses GDR */
    if (p100vram_select_policy(P100VRAM_IO_WRITE, t->gdr_write_threshold_bytes - 1, t)
        != P100VRAM_POLICY_HYBRID) return 2;
    return 0;
}

const struct test_entry test_policy_entries[] = {
    { "policy_defaults",                      test_policy_defaults },
    { "policy_gdr_read_disabled",             test_policy_gdr_read_disabled_by_default },
    { "policy_write_threshold_is_inclusive",  test_policy_write_threshold_is_inclusive_cuda },
    { NULL, NULL },
};
