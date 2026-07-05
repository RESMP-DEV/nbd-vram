/* gdr_writepath.h - GDRCopy write-path wrapper (Stage 8) */
#ifndef P100VRAM_GDR_WRITEPATH_H
#define P100VRAM_GDR_WRITEPATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct p100vram_gdr_handle;

int  p100vram_gdr_open (struct p100vram_gdr_handle **out);
void p100vram_gdr_close(struct p100vram_gdr_handle *h);

/* Pin a GPU buffer for CPU access. Returns an opaque cookie used by
 * subsequent reads/writes, or negative errno on failure. */
int  p100vram_gdr_pin  (struct p100vram_gdr_handle *h, uint64_t gpu_va,
                        size_t size, uint64_t *cookie);
int  p100vram_gdr_unpin(struct p100vram_gdr_handle *h, uint64_t cookie);

/* Small low-overhead write into BAR-mapped GPU memory. Returns 0 on
 * success or negative errno. Stub returns -ENOSYS until stage 8. */
int  p100vram_gdr_write(struct p100vram_gdr_handle *h, uint64_t cookie,
                        uint64_t offset, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* P100VRAM_GDR_WRITEPATH_H */
