/* p100vram.c - blk-mq block device backed by pinned GPU VRAM (Stage 7)
 *
 * One /dev/p100vram<name> per pinned GPU allocation. Linux handles
 * multi-card striping via swap priority; we do not.
 *
 * Boring rules (from the project plan; do NOT violate in Stage 7):
 *   - No CUDA from kernel space.
 *   - No fake NUMA memory; do not make malloc() allocate into HBM2.
 *   - Copies happen between bio pages and mapped GPU DMA pages.
 *
 * The userspace allocator daemon (kernel/allocator/) owns the CUDA
 * allocation; this module only pins/maps and serves bios. NVIDIA P2P
 * requires a free/invalidation callback - we MUST register it and we
 * MUST honor it (see p100vram_p2p_free_cb).
 *
 * This is a skeleton: every NVIDIA P2P call is gated behind
 * CONFIG_NVIDIA_P2P and the actual pin/copy paths are TODO(stage 7).
 * The module compiles without NVIDIA headers present, loads, creates
 * the control misc device, and refuses I/O cleanly when stubbed.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/genhd.h>
#include <linux/idr.h>
#include <linux/uaccess.h>
#include <linux/list.h>

#include "p100vram_ioctl.h"

#ifdef CONFIG_NVIDIA_P2P
#include <linux/nvidia_p2p.h>
#endif

#define DRV_NAME "p100vram"
#define P100VRAM_CONTROL_NAME "p100vram-control"
#define P100VRAM_MINORS 16

MODULE_LICENSE("GPL");
MODULE_AUTHOR("p100vram-shelf");
MODULE_DESCRIPTION("Tesla P100 HBM2 memory shelf block device (Stage 7 skeleton)");
MODULE_INFO(intree, "Y");
MODULE_VERSION("0.1.0-skeleton");

/* -------------------------------------------------------------------------
 * Per-disk state
 * ---------------------------------------------------------------------- */

struct p100vram_disk {
    char name[32];
    int    idr_id;                   /* slot in g_disk_idr            */
    struct gendisk *gd;
    struct blk_mq_tag_set tag_set;
    spinlock_t lock;                 /* protects state + dying count  */
    enum p100vram_disk_state state;

    /* Backing allocation (Stage 7). The allocator daemon fills these. */
    uint64_t gpu_va;
    uint64_t size;
    uint32_t gpu_index;

#ifdef CONFIG_NVIDIA_P2P
    struct nvidia_p2p_page_table *p2p_pages;
    struct nvidia_p2p_dma_mapping *p2p_dma;
    void *p2p_token;                 /* passed back to the free cb    */
#endif
    struct list_head list;
};

static DEFINE_MUTEX(g_lock);
static DEFINE_IDR(g_disk_idr);
static struct list_head g_disks = LIST_HEAD_INIT(g_disks);

/* -------------------------------------------------------------------------
 * blk-mq ops - skeleton
 *
 * The real Stage 7 body iterates the bio, computes the GPU DMA address
 * for each sector, and copies. The skeleton just fails I/O with
 * BLK_STS_IOERR so the device is non-functional but well-behaved.
 * ---------------------------------------------------------------------- */

static blk_status_t p100vram_queue_rq(struct blk_mq_hw_ctx *hctx,
                                      const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct p100vram_disk *disk = rq->q->queuedata;
    unsigned long flags;

    /* TODO(stage 7): pin bio pages, map to GPU DMA, copy. */

    spin_lock_irqsave(&disk->lock, flags);
    if (disk->state != P100VRAM_DISK_LIVE) {
        spin_unlock_irqrestore(&disk->lock, flags);
        blk_mq_end_request(rq, BLK_STS_IOERR);
        return BLK_STS_IOERR;
    }
    spin_unlock_irqrestore(&disk->lock, flags);

    /* Skeleton: no backing store yet. Fail cleanly so swap never
     * believes a write was durable when it was not. */
    blk_mq_end_request(rq, BLK_STS_IOERR);
    return BLK_STS_IOERR;
}

static int p100vram_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
                              unsigned int hctx_idx)
{
    (void)hctx; (void)data; (void)hctx_idx;
    return 0;
}

static const struct blk_mq_ops p100vram_mq_ops = {
    .queue_rq  = p100vram_queue_rq,
    .init_hctx = p100vram_init_hctx,
};

/* -------------------------------------------------------------------------
 * NVIDIA P2P free/invalidation callback - THE safety hook
 *
 * NVIDIA may call this at any time (GPU reset, driver unload, ECC fatal).
 * The disk MUST transition DYING -> DEAD, freeze the queue, and fail all
 * outstanding and future I/O. Never silently continue - failing to
 * honor the callback corrupts live swap pages and MCE-kills processes.
 * ---------------------------------------------------------------------- */

#ifdef CONFIG_NVIDIA_P2P
static void p100vram_p2p_free_cb(void *data)
{
    struct p100vram_disk *disk = data;
    unsigned long flags;

    pr_warn(DRV_NAME ": %s: NVIDIA P2P invalidation fired; failing I/O\n",
            disk->name);

    spin_lock_irqsave(&disk->lock, flags);
    disk->state = P100VRAM_DISK_DYING;
    spin_unlock_irqrestore(&disk->lock, flags);

    /* TODO(stage 7): blk_mq_freeze_queue + drain in-flight, then
     * transition to DEAD and clear p2p_pages. The free of the page
     * table itself is owned by nvidia_p2p_put_pages; we drop our ref
     * after the queue is drained. */
    blk_freeze_queue(disk->gd->queue);
    /* On real impl: nvidia_p2p_put_pages(...); disk->p2p_pages = NULL; */
}
#endif /* CONFIG_NVIDIA_P2P */

/* -------------------------------------------------------------------------
 * Disk create / destroy
 * ---------------------------------------------------------------------- */

static int p100vram_create_disk(const struct p100vram_create_disk *req)
{
    struct p100vram_disk *disk;
    int ret;

    if (req->size == 0 || (req->size & 0xFFF) || req->name[0] == '\0')
        return -EINVAL;
    if (strnlen(req->name, sizeof(req->name)) >= sizeof(req->name))
        return -EINVAL;

    disk = kzalloc(sizeof(*disk), GFP_KERNEL);
    if (!disk)
        return -ENOMEM;

    strscpy(disk->name, req->name, sizeof(disk->name));
    disk->gpu_va    = req->gpu_va;
    disk->size      = req->size;
    disk->gpu_index = req->gpu_index;
    disk->state     = P100VRAM_DISK_LIVE;
    spin_lock_init(&disk->lock);
    INIT_LIST_HEAD(&disk->list);

    /* Pin the GPU pages. Stubbed unless we have the NVIDIA headers. */
#ifdef CONFIG_NVIDIA_P2P
    disk->p2p_token = disk;
    ret = nvidia_p2p_get_pages((uint64_t)disk->gpu_va, (uint64_t)disk->size,
                               &disk->p2p_pages, p100vram_p2p_free_cb,
                               disk->p2p_token);
    /* TODO(stage 7): also acquire the dma_mapping via
     * nvidia_p2p_dma_map_pages for the PCI direction we issue. */
    if (ret) {
        pr_err(DRV_NAME ": nvidia_p2p_get_pages failed: %d\n", ret);
        kfree(disk);
        return ret;
    }
#else
    pr_warn(DRV_NAME ": built without CONFIG_NVIDIA_P2P; %s is a no-op disk\n",
            disk->name);
#endif

    disk->tag_set.ops         = &p100vram_mq_ops;
    disk->tag_set.nr_hw_queues= 1;
    disk->tag_set.queue_depth = 128;
    disk->tag_set.numa_node   = NUMA_NO_NODE;
    disk->tag_set.flags       = BLK_MQ_F_SHOULD_MERGE;
    disk->tag_set.driver_data = disk;

    ret = blk_mq_alloc_tag_set(&disk->tag_set);
    if (ret) {
        pr_err(DRV_NAME ": blk_mq_alloc_tag_set failed: %d\n", ret);
        goto err_put_pages;
    }

    disk->gd = blk_mq_alloc_disk(&disk->tag_set, disk);
    if (IS_ERR(disk->gd)) {
        ret = PTR_ERR(disk->gd);
        goto err_free_tag_set;
    }

    disk->gd->major       = 0;            /* dynamic major per disk   */
    disk->gd->first_minor = 0;
    disk->gd->minors      = 1;
    disk->gd->fops        = NULL;         /* TODO(stage 7): block fops */
    disk->gd->private_data= disk;
    strscpy(disk->gd->disk_name, disk->name, DISK_NAME_LEN);
    set_capacity(disk->gd, disk->size >> SECTOR_SHIFT);
    blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->gd->queue);
    blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, disk->gd->queue);

    /* TODO(stage 7): set dma_alignment + virt_boundary to 4 KiB so the
     * block layer gives us page-aligned bios suitable for P2P DMA. */

    mutex_lock(&g_lock);
    ret = idr_alloc(&g_disk_idr, disk, 0, P100VRAM_MINORS, GFP_KERNEL);
    if (ret >= 0) {
        disk->idr_id = ret;
        list_add_tail(&disk->list, &g_disks);
    }
    mutex_unlock(&g_lock);
    if (ret < 0)
        goto err_put_disk;

    add_disk(disk->gd);
    pr_info(DRV_NAME ": created /dev/%s size=%llu gpu=%u\n",
            disk->name, (unsigned long long)disk->size, disk->gpu_index);
    return 0;

err_put_disk:
    put_disk(disk->gd);
err_free_tag_set:
    blk_mq_free_tag_set(&disk->tag_set);
err_put_pages:
#ifdef CONFIG_NVIDIA_P2P
    if (disk->p2p_pages)
        nvidia_p2p_put_pages((uint64_t)disk->gpu_va, (uint64_t)disk->size,
                             disk->p2p_pages, disk->p2p_token);
#endif
    kfree(disk);
    return ret;
}

static void p100vram_destroy_disk_locked(struct p100vram_disk *disk)
{
    list_del(&disk->list);
    idr_remove(&g_disk_idr, disk->idr_id);
}

/* -------------------------------------------------------------------------
 * Control device ioctl
 * ---------------------------------------------------------------------- */

static long p100vram_ctl_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    void __user *u = (void __user *)arg;

    switch (cmd) {
    case P100VRAM_IOC_CREATE: {
        struct p100vram_create_disk req;
        if (copy_from_user(&req, u, sizeof(req)))
            return -EFAULT;
        return p100vram_create_disk(&req);
    }
    case P100VRAM_IOC_DESTROY: {
        /* TODO(stage 7): look up by name, freeze, invalidate, remove. */
        return -ENOSYS;
    }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations p100vram_ctl_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = p100vram_ctl_ioctl,
    .compat_ioctl   = p100vram_ctl_ioctl,
    .llseek         = noop_llseek,
};

static struct miscdevice p100vram_ctl_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = P100VRAM_CONTROL_NAME,
    .fops  = &p100vram_ctl_fops,
    .mode  = 0600,
};

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static int __init p100vram_init(void)
{
    int ret;

    pr_info(DRV_NAME ": loading (Stage 7 skeleton, nvidia_p2p=%s)\n",
#ifdef CONFIG_NVIDIA_P2P
            "yes"
#else
            "no - disks will be no-op"
#endif
            );

    ret = misc_register(&p100vram_ctl_dev);
    if (ret) {
        pr_err(DRV_NAME ": misc_register failed: %d\n", ret);
        return ret;
    }
    pr_info(DRV_NAME ": control device at /dev/%s\n", P100VRAM_CONTROL_NAME);
    return 0;
}

static void __exit p100vram_exit(void)
{
    struct p100vram_disk *disk, *tmp;

    mutex_lock(&g_lock);
    list_for_each_entry_safe(disk, tmp, &g_disks, list) {
        /* TODO(stage 7): graceful freeze + nvidia_p2p_put_pages. */
        pr_warn(DRV_NAME ": %s still live at unload\n", disk->name);
        if (disk->gd) {
            del_gendisk(disk->gd);
            put_disk(disk->gd);
        }
        blk_mq_free_tag_set(&disk->tag_set);
        p100vram_destroy_disk_locked(disk);
        kfree(disk);
    }
    idr_destroy(&g_disk_idr);
    mutex_unlock(&g_lock);

    misc_deregister(&p100vram_ctl_dev);
    pr_info(DRV_NAME ": unloaded\n");
}

module_init(p100vram_init);
module_exit(p100vram_exit);
