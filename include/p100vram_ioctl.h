/* p100vram_ioctl.h - UAPI for the p100vram blk-mq kernel module (Stage 7)
 *
 * Shared between the kmod, the userspace allocator daemon, and tests.
 * #ifdef __KERNEL__ guards keep the userspace side portable.
 *
 * Control device: /dev/p100vram-control  (misc-device; one per host)
 * Block devices:  /dev/p100vram<name>    (one per pinned GPU allocation)
 */
#ifndef P100VRAM_IOCTL_H
#define P100VRAM_IOCTL_H

#include <stdint.h>

#ifdef __linux__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
/* macOS / non-Linux test build: provide compatible typedefs so the
 * header parses for unit tests of struct layout. The ioctl numbers are
 * illustrative only off-Linux. */
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#define _IOWR(magic, nr, type)  ((unsigned int)(((magic) << 8) | (nr)))
#endif

#define P100VRAM_IOCTL_MAGIC 'P'

/* ioctls on /dev/p100vram-control */
struct p100vram_create_disk {
    __u64 gpu_va;        /* device virtual address of the pinned buffer */
    __u64 size;          /* size in bytes; must be 4 KiB-aligned */
    __u32 gpu_index;     /* CUDA device index the buffer lives on     */
    __u32 flags;         /* reserved, must be 0                       */
    char   name[32];     /* /dev/p100vram<name>; NUL-terminated       */
};

struct p100vram_destroy_disk {
    char name[32];       /* /dev/p100vram<name> to tear down          */
};

#define P100VRAM_IOC_CREATE   _IOWR(P100VRAM_IOCTL_MAGIC, 1, struct p100vram_create_disk)
#define P100VRAM_IOC_DESTROY  _IOWR(P100VRAM_IOCTL_MAGIC, 2, struct p100vram_destroy_disk)

/* Per-disk flags exported via sysfs / debug for the survival tests
 * (Stage 7 gate: survives mkswap, swapon, pressure, swapoff, daemon
 * restart, card reset). */
enum p100vram_disk_state {
    P100VRAM_DISK_LIVE  = 0,
    P100VRAM_DISK_DYING = 1,   /* invalidation fired; draining I/O    */
    P100VRAM_DISK_DEAD  = 2,   /* all I/O failed; safe to remove      */
};

/* Compile-time assertions on layout, checked by test/unit/test_ioctl_layout.c */
#define P100VRAM_CREATE_DISK_EXPECTED_SIZE 56   /* 8+8+4+4 + 32 */

#endif /* P100VRAM_IOCTL_H */
