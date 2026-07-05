/* p100vram-sidecar.c - per-GPU CUDA sidecar RPC daemon (Stage 9)
 *
 * One daemon per GPU, listening on /run/p100vram-sidecar<N>.sock.
 * Receives RPC ops (hash/compress/scan/dedup/prefetch), dispatches to
 * the CUDA kernel wrappers, returns results.
 *
 * The point: keep the block device boring, and make this the place
 * where clever near-data compute happens so we ship results, not pages.
 *
 * Skeleton status:
 *   - arg parsing, socket setup, accept loop: present
 *   - CUDA dlopen, pinned buffers, kernel dispatch: TODO(stage 9)
 *   - The CUDA kernel wrappers (kernels.o / kernels_stub.o) are linked
 *     in; the stub returns success so the daemon's plumbing can be
 *     exercised end-to-end on the Mac.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

#include "p100vram_rpc.h"
#include "p100vram_kernels.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int bind_sock(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    unlink(path);   /* stale socket from a previous run */
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    if (listen(fd, 8) != 0) { close(fd); return -1; }
    chmod(path, 0600);
    return fd;
}

/* Read exactly n bytes. Returns 0 on success, -1 on EOF/error. */
static int read_n(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int write_n(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* Forward decl: handle_client dispatches via send_reply, defined below. */
static int send_reply(int cfd, uint16_t op, uint64_t req_id,
                      const void *body, uint64_t body_bytes);

static int handle_client(int cfd) {
    uint8_t hbuf[24];
    struct p100vram_rpc_hdr h;
    if (read_n(cfd, hbuf, sizeof(hbuf)) != 0) return -1;
    if (p100vram_rpc_decode_hdr(&h, hbuf, sizeof(hbuf)) < 0) return -1;
    if (p100vram_rpc_validate_hdr(&h) != 0) return -1;

    /* Dispatch every advertised opcode to its kernel wrapper so the
     * wiring is real even in stub form (each wrapper returns 0 + zeroed
     * output today). The real Stage 9 path will read op-specific bodies,
     * allocate scratch, launch the kernel, and write real results.
     *
     * Each case below reads its op-specific request body before replying,
     * so a client using COMPRESS/SCAN/DEDUP/PREFETCH gets a dispatched
     * response rather than falling through to a generic empty reply. */
    uint16_t op = h.op;
    uint64_t req_id = h.req_id;

    switch (op) {
    case P100VRAM_OP_HASH_PAGES: {
        struct p100vram_rpc_hash_pages_req req;
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        uint64_t digest = 0;
        p100vram_kern_hash_pages(req.gpu_va + req.offset, req.length,
                                 req.algo, &digest);
        struct p100vram_rpc_hash_pages_rep rep = { .digest = digest };
        send_reply(cfd, op, req_id, &rep, sizeof(rep));
        break;
    }
    case P100VRAM_OP_COMPRESS_PAGES: {
        struct p100vram_rpc_hash_pages_req req;  /* same layout: region + length */
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        uint64_t out_bytes = 0;
        /* Stub: out_va 0, out_cap 0 -> wrapper returns 0 bytes. */
        p100vram_kern_compress_pages(req.gpu_va + req.offset, req.length,
                                     0, 0, &out_bytes);
        struct { uint64_t out_bytes; } rep = { out_bytes };
        send_reply(cfd, op, req_id, &rep, sizeof(rep));
        break;
    }
    case P100VRAM_OP_SCAN_VECTORS: {
        struct p100vram_rpc_hash_pages_req req;  /* region header */
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        float best = 0.f;
        uint64_t idx = 0;
        p100vram_kern_scan_vectors(req.gpu_va + req.offset, 0, 0, 0,
                                   &best, &idx);
        struct { float best; uint64_t idx; } rep = { best, idx };
        send_reply(cfd, op, req_id, &rep, sizeof(rep));
        break;
    }
    case P100VRAM_OP_DEDUP_REGION: {
        struct p100vram_rpc_hash_pages_req req;
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        p100vram_kern_dedup_region(req.gpu_va + req.offset, req.length, 0, 0);
        send_reply(cfd, op, req_id, NULL, 0);
        break;
    }
    case P100VRAM_OP_PREFETCH: {
        struct p100vram_rpc_hash_pages_req req;
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        p100vram_kern_prefetch_region(0, req.gpu_va + req.offset, req.length);
        send_reply(cfd, op, req_id, NULL, 0);
        break;
    }
    default:
        /* validate_hdr already rejected unknown opcodes, so this is
         * unreachable; keep it defensive. */
        return -1;
    }
    return 0;
}

/* Encode the reply header + optional body and write them. body may be
 * NULL when body_bytes is 0. */
static int send_reply(int cfd, uint16_t op, uint64_t req_id,
                      const void *body, uint64_t body_bytes) {
    struct p100vram_rpc_hdr rh = {
        .magic = P100VRAM_RPC_MAGIC, .version = P100VRAM_RPC_VERSION,
        .op = op, .req_id = req_id, .body_bytes = body_bytes,
    };
    uint8_t out[24];
    p100vram_rpc_encode_hdr(&rh, out, sizeof(out));
    if (write_n(cfd, out, sizeof(out)) != 0) return -1;
    if (body_bytes && body && write_n(cfd, body, body_bytes) != 0) return -1;
    return 0;
}

static void usage(const char *a0) {
    fprintf(stderr, "Usage: %s --gpu N\n  One sidecar per GPU.\n", a0);
}

int main(int argc, char **argv) {
    int gpu = -1;
    static const struct option opts[] = {
        { "gpu", required_argument, NULL, 'g' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "g:h", opts, NULL)) != -1) {
        if (c == 'g') gpu = atoi(optarg);
        else if (c == 'h') { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (gpu < 0) { usage(argv[0]); return 2; }

    struct sigaction sa = { .sa_handler = on_sig };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    char *sock_path = p100vram_sidecar_sock_path((unsigned)gpu);
    if (!sock_path) { fprintf(stderr, "OOM\n"); return 1; }
    int lfd = bind_sock(sock_path);
    if (lfd < 0) {
        fprintf(stderr, "[sidecar] bind %s: %s\n", sock_path, strerror(errno));
        free(sock_path);
        return 1;
    }
    fprintf(stderr, "[sidecar] gpu %d listening on %s\n", gpu, sock_path);

    while (!g_stop) {
        struct pollfd pf = { .fd = lfd, .events = POLLIN };
        if (poll(&pf, 1, 1000) <= 0) continue;
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        handle_client(cfd);
        close(cfd);
    }
    close(lfd);
    unlink(sock_path);
    free(sock_path);
    return 0;
}
