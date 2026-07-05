/* p100vram_rpc.h - protocol for the per-GPU CUDA sidecar (Stage 9)
 *
 * One sidecar daemon per GPU listens on /run/p100vram-sidecar<N>.sock
 * and serves near-data compute ops that the block-device layer does not
 * touch. The framing is intentionally simple: a fixed header followed
 * by an op-specific request body, and a fixed header + body reply.
 *
 * This header is pure data layout so it can be unit-tested anywhere.
 */
#ifndef P100VRAM_RPC_H
#define P100VRAM_RPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic + version let the daemon and clients reject mismatched builds. */
#define P100VRAM_RPC_MAGIC    UINT32_C(0x50313030)   /* "P100" */
#define P100VRAM_RPC_VERSION  1

/* Opcodes (Stage 9 kernel list, low-risk first). */
enum p100vram_rpc_op {
    P100VRAM_OP_HASH_PAGES    = 1,   /* page checksum                  */
    P100VRAM_OP_COMPRESS_PAGES= 2,   /* page compression probe         */
    P100VRAM_OP_SCAN_VECTORS  = 3,   /* vector dot/cosine scan         */
    P100VRAM_OP_DEDUP_REGION  = 4,   /* dedup hash                     */
    P100VRAM_OP_PREFETCH      = 5,   /* prefetch_region                */
};

/* Common status codes. */
enum p100vram_rpc_status {
    P100VRAM_RPC_OK            = 0,
    P100VRAM_RPC_EINVAL        = 1,   /* bad op / size / version     */
    P100VRAM_RPC_ENOMEM        = 2,   /* server OOM                   */
    P100VRAM_RPC_EGPU          = 3,   /* CUDA / GPU failure           */
    P100VRAM_RPC_EDEAD         = 4,   /* GPU backing gone (invalid.) */
};

/* All integers are host-endian on the Unix socket (same host only).
 * If we ever cross hosts, add byte-swapping at the boundary. */
struct p100vram_rpc_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t op;          /* enum p100vram_rpc_op     */
    uint64_t req_id;      /* client-chosen, echoed    */
    uint64_t body_bytes;  /* payload following header */
};

/* Hash-pages body (the simplest op). Other ops reuse this layout with
 * an op-specific interpretation of result_bytes in the reply. */
struct p100vram_rpc_hash_pages_req {
    uint64_t gpu_va;       /* device VA region to hash          */
    uint64_t offset;       /* byte offset within the allocation */
    uint64_t length;       /* bytes to hash, multiple of 4 KiB  */
    uint32_t algo;         /* 0 = crc32c, 1 = xxhash64          */
    uint32_t _pad;
};

struct p100vram_rpc_hash_pages_rep {
    uint64_t digest;       /* single rolling digest over region */
};

/* Encoders / decoders used by both daemon and unit tests. They are the
 * only on-the-wire surface, so they are tested in isolation. */
int p100vram_rpc_encode_hdr(const struct p100vram_rpc_hdr *h, uint8_t *buf, size_t cap);
int p100vram_rpc_decode_hdr(struct p100vram_rpc_hdr *h, const uint8_t *buf, size_t len);
int p100vram_rpc_validate_hdr(const struct p100vram_rpc_hdr *h);

/* Sidecar socket path helper. Caller frees the result. */
char *p100vram_sidecar_sock_path(unsigned gpu_index);

#ifdef __cplusplus
}
#endif

#endif /* P100VRAM_RPC_H */
