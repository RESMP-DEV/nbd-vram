/* nbd-vram.c - NBD server backed by GPU VRAM via CUDA
 *
 * Implements NBD fixed-newstyle protocol over a Unix socket.
 * No NVIDIA P2P or kernel symbols needed - uses cuMemcpyHtoDAsync/DtoHAsync.
 *
 * Compile: gcc -O2 -o nbd-vram nbd-vram.c -ldl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <endian.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <pthread.h>

/* PR_SET_IO_FLUSHER (Linux 5.6+) may be absent from older headers */
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

/* -------------------------------------------------------------------------
 * CUDA driver API (dynamic load)
 * ---------------------------------------------------------------------- */

typedef int                CUresult;
typedef int                CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st    *CUcontext;
typedef struct CUstream_st *CUstream;

#define CUDA_SUCCESS           0
#define CU_CTX_SCHED_AUTO      0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuCtxSetCurrent)(CUcontext);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoDAsync)(CUdeviceptr, const void *, size_t, CUstream);
typedef CUresult (*pfn_cuMemcpyDtoHAsync)(void *, CUdeviceptr, size_t, CUstream);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned int);
typedef CUresult (*pfn_cuStreamDestroy)(CUstream);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuMemAllocHost)(void **, size_t);
typedef CUresult (*pfn_cuMemFreeHost)(void *);
typedef CUresult (*pfn_cuGetErrorString)(CUresult, const char **);

static void                  *g_libcuda;
static pfn_cuInit              _cuInit;
static pfn_cuDeviceGet         _cuDeviceGet;
static pfn_cuCtxCreate         _cuCtxCreate;
static pfn_cuCtxDestroy        _cuCtxDestroy;
static pfn_cuCtxSetCurrent     _cuCtxSetCurrent;
static pfn_cuMemAlloc          _cuMemAlloc;
static pfn_cuMemFree           _cuMemFree;
static pfn_cuMemcpyHtoDAsync   _cuMemcpyHtoDAsync;
static pfn_cuMemcpyDtoHAsync   _cuMemcpyDtoHAsync;
static pfn_cuStreamCreate      _cuStreamCreate;
static pfn_cuStreamDestroy     _cuStreamDestroy;
static pfn_cuStreamSynchronize _cuStreamSynchronize;
static pfn_cuMemAllocHost      _cuMemAllocHost;
static pfn_cuMemFreeHost       _cuMemFreeHost;
static pfn_cuGetErrorString    _cuGetErrorString;

#define LOAD_SYM(h, name, pfn) do { \
    pfn = dlsym(h, name); \
    if (!pfn) { fprintf(stderr, "dlsym(%s) failed\n", name); return -1; } \
} while (0)

static int load_libcuda(void) {
    const char *paths[] = { "libcuda.so.1",
                             "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
                             "/usr/lib64/libcuda.so.1", NULL };
    for (int i = 0; paths[i]; i++) {
        g_libcuda = dlopen(paths[i], RTLD_NOW);
        if (g_libcuda) { printf("[nbd-vram] loaded %s\n", paths[i]); break; }
    }
    if (!g_libcuda) { fprintf(stderr, "[nbd-vram] cannot load libcuda.so.1\n"); return -1; }
    LOAD_SYM(g_libcuda, "cuInit",                _cuInit);
    LOAD_SYM(g_libcuda, "cuDeviceGet",           _cuDeviceGet);
    LOAD_SYM(g_libcuda, "cuCtxCreate_v2",        _cuCtxCreate);
    LOAD_SYM(g_libcuda, "cuCtxDestroy_v2",       _cuCtxDestroy);
    LOAD_SYM(g_libcuda, "cuCtxSetCurrent",       _cuCtxSetCurrent);
    LOAD_SYM(g_libcuda, "cuMemAlloc_v2",         _cuMemAlloc);
    LOAD_SYM(g_libcuda, "cuMemFree_v2",          _cuMemFree);
    LOAD_SYM(g_libcuda, "cuMemcpyHtoDAsync_v2",  _cuMemcpyHtoDAsync);
    LOAD_SYM(g_libcuda, "cuMemcpyDtoHAsync_v2",  _cuMemcpyDtoHAsync);
    LOAD_SYM(g_libcuda, "cuStreamCreate",         _cuStreamCreate);
    LOAD_SYM(g_libcuda, "cuStreamDestroy_v2",     _cuStreamDestroy);
    LOAD_SYM(g_libcuda, "cuStreamSynchronize",    _cuStreamSynchronize);
    LOAD_SYM(g_libcuda, "cuMemAllocHost_v2",      _cuMemAllocHost);
    LOAD_SYM(g_libcuda, "cuMemFreeHost",          _cuMemFreeHost);
    LOAD_SYM(g_libcuda, "cuGetErrorString",       _cuGetErrorString);
    return 0;
}

static const char *cuda_err(CUresult r) {
    const char *s = NULL;
    if (_cuGetErrorString) _cuGetErrorString(r, &s);
    return s ? s : "unknown";
}

#define CUDA_CHECK(call) do { \
    CUresult _r = (call); \
    if (_r != CUDA_SUCCESS) { \
        fprintf(stderr, "[nbd-vram] " #call " failed: %s (%d)\n", cuda_err(_r), _r); \
        return -1; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic */
#define NBD_MAGIC_INIT     UINT64_C(0x4e42444d41474943)  /* "NBDMAGIC" */
#define NBD_IHAVEOPT       UINT64_C(0x49484156454f5054)  /* "IHAVEOPT" */
#define NBD_OPT_REP_MAGIC  UINT64_C(0x3e889045565a9)

/* Server handshake flags */
#define NBD_FLAG_FIXED_NEWSTYLE  0x0001
#define NBD_FLAG_NO_ZEROES       0x0002

/* Client handshake flags */
#define NBD_FLAG_C_FIXED_NEWSTYLE 0x00000001
#define NBD_FLAG_C_NO_ZEROES      0x00000002

/* Options (client→server) */
#define NBD_OPT_EXPORT_NAME  1
#define NBD_OPT_ABORT        2
#define NBD_OPT_LIST         3
#define NBD_OPT_INFO         6
#define NBD_OPT_GO           7

/* Option replies (server→client) */
#define NBD_REP_ACK          1
#define NBD_REP_SERVER       2
#define NBD_REP_INFO         3
#define NBD_REP_FLAG_ERROR   UINT32_C(0x80000000)
#define NBD_REP_ERR_UNSUP    (NBD_REP_FLAG_ERROR | 1)

/* Info types */
#define NBD_INFO_EXPORT      0

/* Transmission flags (per-export) */
#define NBD_FLAG_HAS_FLAGS      0x0001
#define NBD_FLAG_SEND_FLUSH     0x0004
#define NBD_FLAG_CAN_MULTI_CONN 0x0100

/* Transmission request magic */
#define NBD_REQUEST_MAGIC    0x25609513
#define NBD_RESPONSE_MAGIC   0x67446698

/* Commands */
#define NBD_CMD_READ         0
#define NBD_CMD_WRITE        1
#define NBD_CMD_DISC         2
#define NBD_CMD_FLUSH        3
#define NBD_CMD_TRIM         4

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

static int recv_all(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int drain(int fd, uint32_t len) {
    char buf[4096];
    while (len > 0) {
        uint32_t chunk = (len > sizeof(buf)) ? sizeof(buf) : len;
        if (recv_all(fd, buf, chunk) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * NBD option reply helpers
 * ---------------------------------------------------------------------- */

static int send_opt_reply(int fd, uint32_t opt, uint32_t reply_type,
                           const void *data, uint32_t data_len)
{
    struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply_type;
        uint32_t len;
    } __attribute__((packed)) hdr;

    hdr.magic      = htobe64(NBD_OPT_REP_MAGIC);
    hdr.opt        = htonl(opt);
    hdr.reply_type = htonl(reply_type);
    hdr.len        = htonl(data_len);

    if (send_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (data_len > 0 && send_all(fd, data, data_len) != 0) return -1;
    return 0;
}

static int send_export_info(int fd, uint32_t opt, uint64_t size, uint16_t tx_flags)
{
    struct {
        uint16_t info_type;   /* NBD_INFO_EXPORT = 0 */
        uint64_t export_size;
        uint16_t tx_flags;
    } __attribute__((packed)) info;

    info.info_type   = htons(NBD_INFO_EXPORT);
    info.export_size = htobe64(size);
    info.tx_flags    = htons(tx_flags);

    return send_opt_reply(fd, opt, NBD_REP_INFO, &info, sizeof(info));
}

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle handshake
 * ---------------------------------------------------------------------- */

static int nbd_handshake(int fd, uint64_t vram_size)
{
    /* Phase 1: server greeting */
    struct {
        uint64_t magic1;
        uint64_t magic2;
        uint16_t srv_flags;
    } __attribute__((packed)) greeting;

    greeting.magic1    = htobe64(NBD_MAGIC_INIT);
    greeting.magic2    = htobe64(NBD_IHAVEOPT);
    greeting.srv_flags = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (send_all(fd, &greeting, sizeof(greeting)) != 0) return -1;

    /* Phase 2: client flags */
    uint32_t client_flags_net;
    if (recv_all(fd, &client_flags_net, 4) != 0) return -1;
    uint32_t client_flags = ntohl(client_flags_net);
    int no_zeroes = !!(client_flags & NBD_FLAG_C_NO_ZEROES);

    /* Phase 3: option haggling */
    for (;;) {
        struct {
            uint64_t ihaveopt;
            uint32_t opt;
            uint32_t opt_len;
        } __attribute__((packed)) opt_hdr;

        if (recv_all(fd, &opt_hdr, sizeof(opt_hdr)) != 0) return -1;
        if (be64toh(opt_hdr.ihaveopt) != NBD_IHAVEOPT) return -1;

        uint32_t opt     = ntohl(opt_hdr.opt);
        uint32_t opt_len = ntohl(opt_hdr.opt_len);

        /* Limit option payload to something sane */
        if (opt_len > 65536) return -1;

        uint16_t tx_flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_CAN_MULTI_CONN;

        switch (opt) {
        case NBD_OPT_EXPORT_NAME:
            /* Drain the export name (we only have one export) */
            if (drain(fd, opt_len) != 0) return -1;
            /* Reply: export size + tx_flags [+ 124 zeros if needed] */
            {
                struct {
                    uint64_t size;
                    uint16_t tx_flags;
                } __attribute__((packed)) info;
                info.size     = htobe64(vram_size);
                info.tx_flags = htons(tx_flags);
                if (send_all(fd, &info, sizeof(info)) != 0) return -1;
                if (!no_zeroes) {
                    char zeros[124] = {0};
                    if (send_all(fd, zeros, sizeof(zeros)) != 0) return -1;
                }
            }
            return 0;  /* transmission begins */

        case NBD_OPT_GO:
        case NBD_OPT_INFO:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_export_info(fd, opt, vram_size, tx_flags) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            if (opt == NBD_OPT_GO)
                return 0;  /* transmission begins */
            break;

        case NBD_OPT_LIST:
            /* One anonymous export */
            if (drain(fd, opt_len) != 0) return -1;
            {
                uint32_t name_len = htonl(0);
                if (send_opt_reply(fd, opt, NBD_REP_SERVER, &name_len, 4) != 0)
                    return -1;
            }
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            break;

        case NBD_OPT_ABORT:
            drain(fd, opt_len);
            send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0);
            return -1;

        default:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ERR_UNSUP, NULL, 0) != 0)
                return -1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Transmission loop
 * ---------------------------------------------------------------------- */

#define DEFAULT_SIZE_MB 7168
#define SIZE_ALIGN      (64 * 1024)
#define SOCK_PATH       "/run/nbd-vram.sock"
#define IO_BUF_SIZE     (4 * 1024 * 1024)
#define NBD_THREADS_MAX     64
#define NBD_THREADS_DEFAULT  4

static CUdeviceptr  g_vram_ptr;
static uint64_t     g_vram_size;
static CUcontext    g_cu_ctx;
static int          g_listen_fd  = -1;
static volatile int g_running    = 1;
static int          g_nbd_threads = NBD_THREADS_DEFAULT;
static volatile int g_client_fds[NBD_THREADS_MAX];

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    /* shutdown active client sockets so threads blocked in recv_all() unblock.
     * Threads waiting for a new connection wake on their own via the poll()
     * timeout in thread_worker - closing the listen fd here would NOT reliably
     * interrupt a thread sitting in accept(), which is what hung shutdown. */
    for (int i = 0; i < g_nbd_threads; i++) {
        int fd = g_client_fds[i];
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
}

static int handle_client(int fd, CUstream stream, char *iobuf)
{
    if (nbd_handshake(fd, g_vram_size) != 0) {
        fprintf(stderr, "[nbd-vram] handshake failed\n");
        return -1;
    }
    printf("[nbd-vram] handshake OK, entering transmission mode\n");

    int ret = 0;
    while (g_running) {
        struct {
            uint32_t magic;
            uint16_t flags;
            uint16_t type;
            uint64_t handle;
            uint64_t from;
            uint32_t len;
        } __attribute__((packed)) req;

        if (recv_all(fd, &req, sizeof(req)) != 0) { ret = -1; break; }

        if (ntohl(req.magic) != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(req.magic));
            ret = -1; break;
        }

        uint16_t cmd    = ntohs(req.type);
        uint64_t handle = req.handle;
        uint64_t offset = be64toh(req.from);
        uint32_t length = ntohl(req.len);

        if (cmd == NBD_CMD_DISC) break;

        /* Bounds check (skip for FLUSH/TRIM which have length=0) */
        uint32_t error = 0;
        if (cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE) {
            if (offset + length > g_vram_size) {
                fprintf(stderr, "[nbd-vram] oob off=%llu len=%u\n",
                        (unsigned long long)offset, length);
                error = EINVAL;
            }
        }

        if (cmd == NBD_CMD_WRITE) {
            /* Must drain data from socket regardless of error */
            uint32_t remaining = length;
            uint64_t voff      = offset;
            while (remaining > 0) {
                uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
                if (recv_all(fd, iobuf, chunk) != 0) { ret = -1; goto done; }
                if (!error) {
                    CUresult r = _cuMemcpyHtoDAsync(g_vram_ptr + voff, iobuf, chunk, stream);
                    if (r != CUDA_SUCCESS) {
                        fprintf(stderr, "[nbd-vram] HtoD failed: %s\n", cuda_err(r));
                        error = EIO;
                    }
                    voff += chunk;
                }
                remaining -= chunk;
            }
            if (!error) _cuStreamSynchronize(stream);

        } else if (cmd == NBD_CMD_FLUSH) {
            _cuStreamSynchronize(stream);
        }
        /* TRIM: just ack success, VRAM doesn't need trimming */

        /* Send response */
        struct {
            uint32_t magic;
            uint32_t error;
            uint64_t handle;
        } __attribute__((packed)) resp;
        resp.magic  = htonl(NBD_RESPONSE_MAGIC);
        resp.error  = htonl(error);
        resp.handle = handle;
        if (send_all(fd, &resp, sizeof(resp)) != 0) { ret = -1; goto done; }
        if (error) goto done;

        if (cmd == NBD_CMD_READ) {
            uint32_t remaining = length;
            uint64_t voff      = offset;
            while (remaining > 0) {
                uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
                CUresult r = _cuMemcpyDtoHAsync(iobuf, g_vram_ptr + voff, chunk, stream);
                if (r != CUDA_SUCCESS) {
                    fprintf(stderr, "[nbd-vram] DtoH failed: %s\n", cuda_err(r));
                    ret = -1; goto done;
                }
                _cuStreamSynchronize(stream);
                if (send_all(fd, iobuf, chunk) != 0) { ret = -1; goto done; }
                remaining -= chunk;
                voff      += chunk;
            }
        }
    }
done:
    return ret;
}

/* -------------------------------------------------------------------------
 * Thread worker - one per NBD connection slot
 * ---------------------------------------------------------------------- */

static void *thread_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    CUstream stream;
    void *iobuf = NULL;
    /* PF_MEMALLOC_NOIO/PF_LOCAL_THROTTLE are per-task; pthread inheritance of
     * PR_SET_IO_FLUSHER is undocumented, so set it per worker too (cheap, the
     * failure case is already logged once from main). */
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
    _cuCtxSetCurrent(g_cu_ctx);
    _cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
    /* Pinned host memory: eliminates CUDA driver staging copy for each DMA */
    _cuMemAllocHost(&iobuf, IO_BUF_SIZE);

    while (g_running) {
        /* Wait for a connection with a 1s timeout so the loop re-checks
         * g_running and exits promptly on SIGTERM (a bare blocking accept()
         * cannot be woken by the signal handler, which hung shutdown for 90s). */
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;            /* timeout or EINTR -> re-check g_running */
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        g_client_fds[idx] = cfd;
        printf("[nbd-vram] client connected\n");
        handle_client(cfd, stream, iobuf);
        g_client_fds[idx] = -1;
        close(cfd);
        printf("[nbd-vram] client disconnected\n");
    }

    if (iobuf) _cuMemFreeHost(iobuf);
    _cuStreamDestroy(stream);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
    CUdevice  cu_dev;
    int       ret    = 1;

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Prevent kernel from paging out our own pages - doing so would route
     * the page fault back through this daemon, deadlocking under swap pressure.
     * Requires LimitMEMLOCK=infinity in the systemd service. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[nbd-vram] mlockall failed (%s) - daemon pages may be swapped, risking deadlock\n",
                strerror(errno));

    /* Mark the daemon as part of the I/O flush path. Sets PF_MEMALLOC_NOIO +
     * PF_LOCAL_THROTTLE so allocations made while servicing a swap write do not
     * recurse back into reclaim/writeback - that recursion is the swap-over-NBD
     * deadlock at zero free RAM. Per-process; set before threads spawn. */
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0)
        fprintf(stderr, "[nbd-vram] PR_SET_IO_FLUSHER failed (%s) - deadlock risk under swap pressure\n",
                strerror(errno));

    if (load_libcuda() != 0) goto out;

    for (int i = 0; i < 10; i++) {
        CUresult r = _cuInit(0);
        if (r == CUDA_SUCCESS) break;
        if (i == 9) {
            fprintf(stderr, "[nbd-vram] cuInit failed: %s\n", cuda_err(r));
            goto out;
        }
        fprintf(stderr, "[nbd-vram] cuInit attempt %d failed, retrying\n", i + 1);
        sleep(2);
    }

    if (_cuDeviceGet(&cu_dev, 0) != CUDA_SUCCESS) goto out;
    if (_cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_AUTO, cu_dev) != CUDA_SUCCESS) goto out;

    const char *env = getenv("VRAM_SETUP_SIZE_MB");
    size_t mb = env ? (size_t)atol(env) : DEFAULT_SIZE_MB;

    /* Back off 512 MiB at a time if the GPU is short on memory (e.g. display compositor loaded) */
    g_vram_ptr = 0;
    while (mb >= 1024) {
        g_vram_size = (mb * 1024ULL * 1024ULL / SIZE_ALIGN) * SIZE_ALIGN;
        printf("[nbd-vram] allocating %llu MiB of VRAM\n",
               (unsigned long long)(g_vram_size >> 20));
        CUresult alloc_r = _cuMemAlloc(&g_vram_ptr, g_vram_size);
        if (alloc_r == CUDA_SUCCESS) break;
        fprintf(stderr, "[nbd-vram] %llu MiB failed (%s), backing off 512 MiB\n",
                (unsigned long long)mb, cuda_err(alloc_r));
        g_vram_ptr = 0;
        mb -= 512;
    }
    if (!g_vram_ptr) {
        fprintf(stderr, "[nbd-vram] all allocation attempts failed\n");
        goto out_cuda;
    }
    printf("[nbd-vram] VRAM at CUDA VA 0x%llx\n", (unsigned long long)g_vram_ptr);

    /* Create Unix socket */
    unlink(SOCK_PATH);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); goto out_cuda; }

    {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            { perror("bind"); goto out_cuda; }
    }
    chmod(SOCK_PATH, 0600);
    if (listen(g_listen_fd, g_nbd_threads + 1) < 0) { perror("listen"); goto out_cuda; }
    /* non-blocking so the poll()+accept() in each worker never blocks if another
     * worker grabbed the pending connection first */
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);

    printf("[nbd-vram] listening on %s (%d threads)\n", SOCK_PATH, g_nbd_threads);

    /* sd_notify READY=1 */
    {
        const char *ns = getenv("NOTIFY_SOCKET");
        if (ns) {
            int nfd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (nfd >= 0) {
                struct sockaddr_un na = { .sun_family = AF_UNIX };
                const char *p = (ns[0] == '@') ? ns + 1 : ns;
                strncpy(na.sun_path, p, sizeof(na.sun_path) - 1);
                const char *msg = "READY=1\n";
                sendto(nfd, msg, strlen(msg), 0, (struct sockaddr *)&na, sizeof(na));
                close(nfd);
            }
        }
    }

    {
        const char *tenv = getenv("VRAM_NBD_THREADS");
        if (tenv) {
            g_nbd_threads = atoi(tenv);
            if (g_nbd_threads < 1) g_nbd_threads = 1;
            if (g_nbd_threads > NBD_THREADS_MAX) g_nbd_threads = NBD_THREADS_MAX;
        }

        pthread_t threads[NBD_THREADS_MAX];
        for (int i = 0; i < g_nbd_threads; i++) g_client_fds[i] = -1;
        for (int i = 0; i < g_nbd_threads; i++)
            pthread_create(&threads[i], NULL, thread_worker, (void *)(intptr_t)i);
        for (int i = 0; i < g_nbd_threads; i++)
            pthread_join(threads[i], NULL);
    }
    ret = 0;

out_cuda:
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    unlink(SOCK_PATH);
    if (g_vram_ptr) _cuMemFree(g_vram_ptr);
    if (g_cu_ctx)   _cuCtxDestroy(g_cu_ctx);
    if (g_libcuda)  dlclose(g_libcuda);
out:
    printf("[nbd-vram] exiting (ret=%d)\n", ret);
    return ret;
}
