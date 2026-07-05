/* rpc_framing.c - implementation of the sidecar RPC encoders/decoders.
 *
 * This is the only piece of the sidecar with real (non-stub) logic in
 * the skeleton, because it is pure data marshalling and the unit tests
 * need it to round-trip on the Mac. The daemon server loop itself
 * stays a stub until Stage 9 wiring lands.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p100vram_rpc.h"

/* All fields little-endian on the wire; we keep it host-endian on the
 * Unix socket and rely on a single-host deployment. */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
}
static uint32_t get_u32(const uint8_t *p) {
    return  (uint32_t)p[0]        | ((uint32_t)p[1] << 8)
          | ((uint32_t)p[2] << 16)| ((uint32_t)p[3] << 24);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define HDR_BYTES 24   /* 4+2+2 + 8 + 8 */

int p100vram_rpc_encode_hdr(const struct p100vram_rpc_hdr *h, uint8_t *buf, size_t cap) {
    if (cap < HDR_BYTES) return -1;
    put_u32(buf + 0,  h->magic);
    put_u16(buf + 4,  h->version);
    put_u16(buf + 6,  h->op);
    /* bytes 8..15: req_id */
    uint64_t r = h->req_id;
    for (int i = 0; i < 8; i++) buf[8 + i] = (uint8_t)(r >> (8 * i));
    /* bytes 16..23: body_bytes */
    uint64_t b = h->body_bytes;
    for (int i = 0; i < 8; i++) buf[16 + i] = (uint8_t)(b >> (8 * i));
    return HDR_BYTES;
}

int p100vram_rpc_decode_hdr(struct p100vram_rpc_hdr *h, const uint8_t *buf, size_t len) {
    if (len < HDR_BYTES) return -1;
    h->magic      = get_u32(buf + 0);
    h->version    = get_u16(buf + 4);
    h->op         = get_u16(buf + 6);
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r |= ((uint64_t)buf[8 + i]) << (8 * i);
    h->req_id = r;
    uint64_t b = 0;
    for (int i = 0; i < 8; i++) b |= ((uint64_t)buf[16 + i]) << (8 * i);
    h->body_bytes = b;
    return HDR_BYTES;
}

int p100vram_rpc_validate_hdr(const struct p100vram_rpc_hdr *h) {
    if (h->magic != P100VRAM_RPC_MAGIC) return -1;
    if (h->version != P100VRAM_RPC_VERSION) return -1;
    switch (h->op) {
    case P100VRAM_OP_HASH_PAGES:
    case P100VRAM_OP_COMPRESS_PAGES:
    case P100VRAM_OP_SCAN_VECTORS:
    case P100VRAM_OP_DEDUP_REGION:
    case P100VRAM_OP_PREFETCH:
        return 0;
    default:
        return -1;
    }
}

char *p100vram_sidecar_sock_path(unsigned gpu_index) {
    /* /run/p100vram-sidecar<N>.sock - matches the daemon's bind path. */
    const char *dir = getenv("P100VRAM_RUN_DIR");
    if (!dir || !dir[0]) dir = "/run";
    size_t n = strlen(dir) + 32;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/p100vram-sidecar%u.sock", dir, gpu_index);
    return p;
}
