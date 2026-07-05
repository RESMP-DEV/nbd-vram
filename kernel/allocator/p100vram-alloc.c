/* p100vram-alloc.c - userspace CUDA allocator for the p100vram kmod (Stage 7)
 *
 * Allocates a CUDA buffer on a GPU, then tells the kernel to pin it and
 * expose it as /dev/p100vram<name>. On exit, frees the buffer; NVIDIA
 * fires the kmod's P2P free callback, which freezes the disk and fails
 * outstanding I/O cleanly.
 *
 * Mirrors nbd-vram's CUDA-load discipline: dlopen libcuda.so.1, no link
 * against the CUDA toolkit, mlockall + PR_SET_IO_FLUSHER so we cannot
 * deadlock the host under memory pressure.
 *
 * Usage: p100vram-alloc --gpu N --size-mb M --name shelfN
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
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <pthread.h>

#include "p100vram_ioctl.h"

#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

/* -------------------------------------------------------------------------
 * CUDA driver API (dynamic load) - same shape as nbd-vram.c
 * ---------------------------------------------------------------------- */

typedef int                CUresult;
typedef int                CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st    *CUcontext;

#define CUDA_SUCCESS 0
#define CU_CTX_SCHED_AUTO 0

typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemGetInfo_v2)(size_t *, size_t *);
typedef CUresult (*pfn_cuDriverGetVersion)(int *);
typedef CUresult (*pfn_cuGetErrorString)(CUresult, const char **);

static void                 *g_libcuda;
static pfn_cuInit             _cuInit;
static pfn_cuDeviceGet        _cuDeviceGet;
static pfn_cuCtxCreate        _cuCtxCreate;
static pfn_cuCtxDestroy       _cuCtxDestroy;
static pfn_cuMemAlloc         _cuMemAlloc;
static pfn_cuMemFree          _cuMemFree;
static pfn_cuMemGetInfo_v2    _cuMemGetInfo;
static pfn_cuDriverGetVersion _cuDriverGetVersion;
static pfn_cuGetErrorString   _cuGetErrorString;

#define LOAD_SYM(h, name, pfn) do { \
    pfn = dlsym(h, name); \
    if (!pfn) { fprintf(stderr, "[p100vram-alloc] dlsym(%s) failed\n", name); return -1; } \
} while (0)

static int load_libcuda(void) {
    const char *paths[] = { "libcuda.so.1",
                            "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
                            "/usr/lib64/libcuda.so.1", NULL };
    for (int i = 0; paths[i]; i++) {
        g_libcuda = dlopen(paths[i], RTLD_NOW);
        if (g_libcuda) { fprintf(stderr, "[p100vram-alloc] loaded %s\n", paths[i]); break; }
    }
    if (!g_libcuda) { fprintf(stderr, "[p100vram-alloc] cannot load libcuda.so.1\n"); return -1; }
    LOAD_SYM(g_libcuda, "cuInit",                _cuInit);
    LOAD_SYM(g_libcuda, "cuDeviceGet",           _cuDeviceGet);
    LOAD_SYM(g_libcuda, "cuCtxCreate_v2",        _cuCtxCreate);
    LOAD_SYM(g_libcuda, "cuCtxDestroy_v2",       _cuCtxDestroy);
    LOAD_SYM(g_libcuda, "cuMemAlloc_v2",         _cuMemAlloc);
    LOAD_SYM(g_libcuda, "cuMemFree_v2",          _cuMemFree);
    LOAD_SYM(g_libcuda, "cuMemGetInfo_v2",       _cuMemGetInfo);
    LOAD_SYM(g_libcuda, "cuDriverGetVersion",    _cuDriverGetVersion);
    LOAD_SYM(g_libcuda, "cuGetErrorString",      _cuGetErrorString);
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
        fprintf(stderr, "[p100vram-alloc] " #call " failed: %s (%d)\n", cuda_err(_r), _r); \
        return -1; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * Allocator
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int allocate_and_register(int gpu, long size_mb, const char *name) {
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr dptr = 0;
    size_t free_b = 0, total_b = 0;
    size_t size = (size_t)size_mb * 1024 * 1024;
    int ctl_fd = -1, ret = -1;

    CUDA_CHECK(_cuInit(0));
    CUDA_CHECK(_cuDeviceGet(&dev, gpu));
    CUDA_CHECK(_cuCtxCreate(&ctx, CU_CTX_SCHED_AUTO, dev));

    _cuMemGetInfo(&free_b, &total_b);
    fprintf(stderr, "[p100vram-alloc] gpu %d free=%zu MiB total=%zu MiB\n",
            gpu, free_b / (1024 * 1024), total_b / (1024 * 1024));

    CUDA_CHECK(_cuMemAlloc(&dptr, size));
    fprintf(stderr, "[p100vram-alloc] allocated %ld MiB at device VA 0x%llx\n",
            size_mb, (unsigned long long)dptr);

    ctl_fd = open("/dev/" P100VRAM_CONTROL_NAME, O_RDWR);
    if (ctl_fd < 0) {
        /* Skeleton-friendly: on the dev box this exists after insmod.
         * On macOS it does not, so report cleanly and free the buffer. */
        fprintf(stderr, "[p100vram-alloc] open /dev/%s: %s\n",
                P100VRAM_CONTROL_NAME, strerror(errno));
        goto out_free;
    }

    struct p100vram_create_disk req = {
        .gpu_va    = (uint64_t)dptr,
        .size      = size,
        .gpu_index = (uint32_t)gpu,
        .flags     = 0,
    };
    strncpy(req.name, name, sizeof(req.name) - 1);

    if (ioctl(ctl_fd, P100VRAM_IOC_CREATE, &req) != 0) {
        fprintf(stderr, "[p100vram-alloc] IOC_CREATE: %s\n", strerror(errno));
        goto out_close;
    }
    fprintf(stderr, "[p100vram-alloc] /dev/p100vram%s ready (size=%zu)\n", name, size);

    /* Hold the allocation live until asked to stop. Race-free wait:
     * block SIGTERM/SIGINT around the check + sigsuspend so a signal
     * delivered between testing g_stop and sleeping is not lost (which
     * plain pause() would miss, leaving the daemon stuck until SIGKILL).
     * NVIDIA's free callback fires in the kmod and fails I/O cleanly. */
    {
        sigset_t mask, oldmask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, &oldmask);
        while (!g_stop)
            sigsuspend(&oldmask);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }

    ret = 0;
out_close:
    close(ctl_fd);
out_free:
    if (dptr) _cuMemFree(dptr);
    _cuCtxDestroy(ctx);
    return ret;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --gpu N --size-mb M --name shelfN [--no-mlock]\n"
        "  Pins M MiB of VRAM on GPU N and registers /dev/p100vram<name>.\n",
        argv0);
}

int main(int argc, char **argv) {
    int gpu = -1, no_mlock = 0;
    long size_mb = 0;
    const char *name = NULL;

    static const struct option opts[] = {
        { "gpu",     required_argument, NULL, 'g' },
        { "size-mb", required_argument, NULL, 's' },
        { "name",    required_argument, NULL, 'n' },
        { "no-mlock",no_argument,       NULL, 'm' },
        { "help",    no_argument,       NULL, 'h' },
        { 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "g:s:n:mh", opts, NULL)) != -1) {
        switch (c) {
        case 'g': gpu = atoi(optarg); break;
        case 's': size_mb = atol(optarg); break;
        case 'n': name = optarg; break;
        case 'm': no_mlock = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (gpu < 0 || size_mb <= 0 || !name) { usage(argv[0]); return 2; }

    /* Deadlock-avoidance, same reasoning as nbd-vram: we must not be
     * paged out, and we must be allowed to write back during reclaim. */
    if (!no_mlock) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            fprintf(stderr, "[p100vram-alloc] mlockall failed (%s) - risk under pressure\n",
                    strerror(errno));
    }
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);

    struct sigaction sa = { .sa_handler = on_sig };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    if (load_libcuda() != 0) return 1;
    return allocate_and_register(gpu, size_mb, name) ? 1 : 0;
}
