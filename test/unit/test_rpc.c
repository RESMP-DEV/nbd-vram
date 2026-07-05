/* test/unit/test_rpc.c - RPC framing unit tests (PR1 foundation).
 *
 * Tests the wire format in include/p100vram_rpc.h against the real
 * rpc_framing.c implementation. In PR1 the implementation comes from
 * a local copy in this dir; once PR4 lands, the Makefile prefers
 * sidecar/daemon/rpc_framing.c and the local copy is removed.
 */
#include <stdint.h>
#include <string.h>

#include "p100vram_rpc.h"
#include "main.h"   /* struct test_entry */

int test_rpc_hdr_roundtrip(void) {
    struct p100vram_rpc_hdr in = {
        .magic = P100VRAM_RPC_MAGIC,
        .version = P100VRAM_RPC_VERSION,
        .op = P100VRAM_OP_HASH_PAGES,
        .req_id = 0xdeadbeefcafebabeULL,
        .body_bytes = 40,
    };
    uint8_t buf[24];
    if (p100vram_rpc_encode_hdr(&in, buf, sizeof(buf)) != 24) return 1;
    struct p100vram_rpc_hdr out;
    if (p100vram_rpc_decode_hdr(&out, buf, sizeof(buf)) != 24) return 2;
    if (out.magic != in.magic || out.version != in.version || out.op != in.op) return 3;
    if (out.req_id != in.req_id || out.body_bytes != in.body_bytes) return 4;
    if (p100vram_rpc_validate_hdr(&out) != 0) return 5;
    return 0;
}

int test_rpc_hdr_rejects_bad_magic(void) {
    struct p100vram_rpc_hdr h = {
        .magic = 0xffffffff, .version = P100VRAM_RPC_VERSION,
        .op = P100VRAM_OP_HASH_PAGES, .req_id = 0, .body_bytes = 0,
    };
    return p100vram_rpc_validate_hdr(&h) == 0 ? 1 : 0;
}

int test_rpc_hdr_rejects_bad_op(void) {
    struct p100vram_rpc_hdr h = {
        .magic = P100VRAM_RPC_MAGIC, .version = P100VRAM_RPC_VERSION,
        .op = 9999, .req_id = 0, .body_bytes = 0,
    };
    return p100vram_rpc_validate_hdr(&h) == 0 ? 1 : 0;
}

int test_rpc_encode_too_small(void) {
    struct p100vram_rpc_hdr h = { .magic = P100VRAM_RPC_MAGIC };
    uint8_t buf[8];
    return p100vram_rpc_encode_hdr(&h, buf, sizeof(buf)) >= 0 ? 1 : 0;
}

const struct test_entry test_rpc_entries[] = {
    { "rpc_hdr_roundtrip",         test_rpc_hdr_roundtrip },
    { "rpc_hdr_rejects_bad_magic", test_rpc_hdr_rejects_bad_magic },
    { "rpc_hdr_rejects_bad_op",    test_rpc_hdr_rejects_bad_op },
    { "rpc_encode_too_small",      test_rpc_encode_too_small },
    { NULL, NULL },
};
