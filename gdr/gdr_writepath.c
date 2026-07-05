/* gdr_writepath.c - GDRCopy low-overhead write path (Stage 8)
 *
 * Thin wrapper over libgdrapi: gdr_open, gdr_pin_buffer, gdr_map. Used
 * by the allocator daemon (and, optionally, the sidecar) for small CPU
 * writes into BAR-mapped GPU memory without a CUDA memcpy launch.
 *
 * The whole TU compiles on macOS: gdr_* calls dispatch through weak
 * stubs in p100vram_compat.h, returning -ENOSYS when the real lib is
 * absent. On Linux with libgdrapi present, link -lgdrapi to override.
 *
 * This is a skeleton: TODO(stage 8) fills in the actual gdr_pin_buffer
 * + gdr_map plumbing once the host has gdrcopy installed.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "p100vram_compat.h"
#include "gdr_writepath.h"

struct p100vram_gdr_handle {
    void *padding;        /* gdr_t on Linux; opaque otherwise */
    int   opened;
};

int p100vram_gdr_open(struct p100vram_gdr_handle **out) {
    struct p100vram_gdr_handle *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    /* TODO(stage 8): h->padding = (void *)gdr_open(); */
    h->opened = 0;       /* stub: not opened */
    *out = h;
    return 0;
}

void p100vram_gdr_close(struct p100vram_gdr_handle *h) {
    if (!h) return;
    /* TODO(stage 8): if (h->opened) gdr_close((gdr_t)h->padding); */
    free(h);
}

int p100vram_gdr_pin(struct p100vram_gdr_handle *h, uint64_t gpu_va,
                     size_t size, uint64_t *cookie) {
    (void)h; (void)gpu_va; (void)size;
    /* TODO(stage 8): gdr_pin_buffer((gdr_t)h->padding, gpu_va, size, 0, &mh);
     *                store mh in a lookup table keyed by *cookie. */
    *cookie = 0;
    return -ENOSYS;
}

int p100vram_gdr_unpin(struct p100vram_gdr_handle *h, uint64_t cookie) {
    (void)h; (void)cookie;
    /* TODO(stage 8): gdr_unpin_buffer((gdr_t)h->padding, mh[cookie]); */
    return -ENOSYS;
}

int p100vram_gdr_write(struct p100vram_gdr_handle *h, uint64_t cookie,
                       uint64_t offset, const void *buf, size_t len) {
    (void)h; (void)cookie; (void)offset; (void)buf; (void)len;
    /* TODO(stage 8): gdr_map(...); memcpy(map_ptr + offset, buf, len);
     *                gdr_unmap(...); gdr_put_sync(...). */
    return -ENOSYS;
}
