/* test/unit/test_policy.c - hybrid policy selection tests (PR3 Stage 8). */
#include "hybrid_policy.h"

int test_policy_defaults(void) {
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

int test_policy_gdr_read_disabled_by_default(void) {
    const struct p100vram_policy_tunables *t = p100vram_default_tunables();
    /* Even a tiny read should be CUDA since gdr_read_enabled=0 */
    return p100vram_select_policy(P100VRAM_IO_READ, 1, t) != P100VRAM_POLICY_ALL_CUDA;
}

#include "main.h"

const struct test_entry test_policy_entries[] = {
    { "policy_defaults",          test_policy_defaults },
    { "policy_gdr_read_disabled", test_policy_gdr_read_disabled_by_default },
    { NULL, NULL },
};
