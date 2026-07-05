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

static int handle_client(int cfd) {
    uint8_t hbuf[24];
    struct p100vram_rpc_hdr h;
    if (read_n(cfd, hbuf, sizeof(hbuf)) != 0) return -1;
    if (p100vram_rpc_decode_hdr(&h, hbuf, sizeof(hbuf)) < 0) return -1;
    if (p100vram_rpc_validate_hdr(&h) != 0) return -1;

    /* Stubbed dispatch: each op calls the matching kernel wrapper,
     * which (in skeleton form) returns 0 + zeroed output. The real
     * Stage 9 path reads the op-specific body, allocates scratch,
     * launches the kernel, and writes the result back. */
    int rc = 0;
    switch (h.op) {
    case P100VRAM_OP_HASH_PAGES: {
        struct p100vram_rpc_hash_pages_req req;
        if (read_n(cfd, &req, sizeof(req)) != 0) return -1;
        uint64_t digest = 0;
        rc = p100vram_kern_hash_pages(req.gpu_va + req.offset, req.length,
                                      req.algo, &digest);
        struct p100vram_rpc_hdr rh = {
            .magic = P100VRAM_RPC_MAGIC, .version = P100VRAM_RPC_VERSION,
            .op = h.op, .req_id = h.req_id,
            .body_bytes = sizeof(struct p100vram_rpc_hash_pages_rep),
        };
        struct p100vram_rpc_hash_pages_rep rep = { .digest = digest };
        uint8_t out[24];
        p100vram_rpc_encode_hdr(&rh, out, sizeof(out));
        write_n(cfd, out, sizeof(out));
        write_n(cfd, &rep, sizeof(rep));
        break;
    }
    default:
        /* Other ops: stub reply with empty body so the client sees a
         * well-formed response and the plumbing is exercisable. */
        rc = 0;
        struct p100vram_rpc_hdr rh = {
            .magic = P100VRAM_RPC_MAGIC, .version = P100VRAM_RPC_VERSION,
            .op = h.op, .req_id = h.req_id, .body_bytes = 0,
        };
        uint8_t out[24];
        p100vram_rpc_encode_hdr(&rh, out, sizeof(out));
        write_n(cfd, out, sizeof(out));
        break;
    }
    (void)rc;
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
