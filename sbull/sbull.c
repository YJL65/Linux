/*
 * 从头开始的示例磁盘驱动程序。
 */

#include <linux/version.h> 	/* LINUX_VERSION_CODE */
#include <linux/blk-mq.h>	
/* https://olegkutkov.me/2020/02/10/linux-block-device-driver/
   https://prog.world/linux-kernel-5-0-we-write-simple-block-device-under-blk-mq/           
   blk-mq 和内核版本 >= 5.0
*/

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* 所有文件系统相关的头文件 */
#include <linux/errno.h>	/* 错误代码 */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>

MODULE_LICENSE("Dual BSD/GPL");

static int sbull_major = 0;  // 主设备号
module_param(sbull_major, int, 0);
static int hardsect_size = 512;  // 硬件扇区大小
module_param(hardsect_size, int, 0);
static int nsectors = 1024;	/* 驱动器的大小 */
module_param(nsectors, int, 0);
static int ndevices = 4;  // 设备数量
module_param(ndevices, int, 0);

/*
 * 我们可以使用的不同“请求模式”。
 */
enum {
    RM_SIMPLE  = 0,	/* 极其简单的请求函数 */
    RM_FULL    = 1,	/* 完整版本 */
    RM_NOQUEUE = 2,	/* 使用 make_request */
};
static int request_mode = RM_SIMPLE;  // 请求模式
module_param(request_mode, int, 0);

/*
 * 次设备号和分区管理。
 */
#define SBULL_MINORS	16
#define MINOR_SHIFT	4
#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT)

/*
 * 我们可以调整硬件扇区大小，但内核总是以小扇区的形式与我们通信。
 */
#define KERNEL_SECTOR_SIZE	512

/*
 * 在这么多空闲时间之后，驱动程序将模拟介质更改。
 */
#define INVALIDATE_DELAY	30*HZ

/*
 * 我们设备的内部表示。
 */
struct sbull_dev {
        int size;                       /* 设备大小（以扇区为单位） */
        u8 *data;                       /* 数据数组 */
        short users;                    /* 用户数量 */
        short media_change;             /* 介质更改标志 */
        spinlock_t lock;                /* 互斥锁 */
    	struct blk_mq_tag_set tag_set;	/* 标签集 */
        struct request_queue *queue;    /* 设备请求队列 */
        Struct gendisk *gd;
struct timer_list timer;        /* 用于模拟介质更改的定时器 */
};

static struct sbull_dev *Devices = NULL;

/**
* 参见 https://github.com/openzfs/zfs/pull/10187/
*/
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
static inline struct request_queue *
blk_generic_alloc_queue(make_request_fn make_request, int node_id)
#else
static inline struct request_queue *
blk_generic_alloc_queue(int node_id)
#endif
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))
    struct request_queue *q = blk_alloc_queue(GFP_KERNEL);
    if (q != NULL)
        blk_queue_make_request(q, make_request);

    return (q);
#elif (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
    return (blk_alloc_queue(make_request, node_id));
#else
    return (blk_alloc_queue(node_id));
#endif
}

/*
 * 处理 I/O 请求。
 */
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector,
                           unsigned long nsect, char *buffer, int write)
{
    if (write)
        memcpy(dev->data + (sector * KERNEL_SECTOR_SIZE), buffer, nsect * KERNEL_SECTOR_SIZE);
    else
        memcpy(buffer, dev->data + (sector * KERNEL_SECTOR_SIZE), nsect * KERNEL_SECTOR_SIZE);
}
/*
 * 简单形式的请求函数。
 */
static blk_status_t sbull_request(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data* bd)   /* 对于 blk-mq */
{
    struct request *req = bd->rq;
    struct sbull_dev *dev = req->rq_disk->private_data;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t pos_sector = blk_rq_pos(req);
    void	*buffer;
    blk_status_t  ret;

    blk_mq_start_request(req);

    if (blk_rq_is_passthrough(req)) {
        printk(KERN_NOTICE "跳过非文件系统请求\n");
        ret = BLK_STS_IOERR;  //-EIO
        goto done;
    }
    rq_for_each_segment(bvec, req, iter)
    {
        size_t num_sector = blk_rq_cur_sectors(req);
        printk(KERN_NOTICE "请求设备 %u 方向 %d 扇区 %lld，数量 %ld\n",
                        (unsigned)(dev - Devices), rq_data_dir(req),
                        pos_sector, num_sector);
        buffer = page_address(bvec.bv_page) + bvec.bv_offset;
        sbull_transfer(dev, pos_sector, num_sector,
                buffer, rq_data_dir(req) == WRITE);
        pos_sector += num_sector;
    }
    ret = BLK_STS_OK;
done:
    blk_mq_end_request(req, ret);
    return ret;
}

/*
 * 传输单个 BIO。
 */
static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
    struct bio_vec bvec;
    struct bvec_iter iter;
    sector_t sector = bio->bi_iter.bi_sector;

    /* 独立处理每个段。 */
    bio_for_each_segment(bvec, bio, iter) {
        char *buffer = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
        sbull_transfer(dev, sector, (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE),
                buffer, bio_data_dir(bio) == WRITE);
        sector += (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE);
        kunmap_atomic(buffer);
    }
    return 0; /* 总是“成功” */
}

/*
 * 传输整个请求。
 */
static int sbull_xfer_request(struct sbull_dev *dev, struct request *req)
{
    struct bio *bio;
    int nsect = 0;
    
    __rq_for_each_bio(bio, req) {
        sbull_xfer_bio(dev, bio);
        nsect += bio->bi_iter.bi_size / KERNEL_SECTOR_SIZE;
    }
    return nsect;
}

/*
 * 更智能的请求函数，“处理聚类”。
 */
static blk_status_t sbull_full_request(struct blk_mq_hw_ctx * hctx, const struct blk_mq_queue_data * bd)
{
    struct request *req = bd->rq;
    int sectors_xferred;
    struct sbull_dev *dev = req->q->queuedata;
    blk_status_t  ret;

    blk_mq_start_request(req);
    if (blk_rq_is_passthrough(req)) {
        printk(KERN_NOTICE "跳过非文件系统请求\n");
        ret = BLK_STS_IOERR; //-EIO;
        goto done;
    }
    sectors_xferred = sbull_xfer_request(dev, req);
    ret = BLK_STS_OK; 
done:
    blk_mq_end_request(req, ret);
    return ret;
}

/*
 * 直接 make_request 版本。
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
static blk_qc_t sbull_make_request(struct request_queue *q, struct bio *bio)
#else
static blk_qc_t sbull_make_request(struct bio *bio)
#endif
{
    struct sbull_dev *dev = bio->bi_disk->private_data;
    int status;

    status = sbull_xfer_bio(dev, bio);
    bio->bi_status = status;
    bio_endio(bio);
    return BLK_QC_T_NONE;
}

/*
 * 打开和关闭。
 */
static int sbull_open(struct block_device *bdev, fmode_t mode)
{
    struct sbull_dev *dev = bdev->bd_disk->private_data;

    del_timer_sync(&dev->timer);
    spin_lock(&dev->lock);
    if (!dev->users) 
    {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0))
        check_disk_change(bdev);
#else
        if(bdev_check_media_change(bdev))
        {
            struct gendisk *gd = bdev->bd_disk;
            const struct block_device_operations *bdo = gd->fops;
            if (bdo && bdo->revalidate_disk)
                bdo->revalidate_disk(gd);
        }
#endif
    }
    dev->users++;
    spin_unlock(&dev->lock);
    return 0;
}

static void sbull_release(struct gendisk *disk, fmode_t mode)
{
    struct sbull_dev *dev = disk->private_data;

    spin_lock(&dev->lock);
    dev->users--;
    if (dev->users < 0)
        printk(KERN_WARNING "sbull: users count is negative!\n");
    spin_unlock(&dev->lock);
}
/*
 * 查找（模拟的）介质更改。
 */
int sbull_media_changed(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;
    return dev->media_change;
}

/*
 * 重新验证。我们在这里不加锁，以避免与 open 死锁。这需要重新评估。
 */
int sbull_revalidate(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;
    if (dev->media_change) {
        dev->media_change = 0;
        memset(dev->data, 0, dev->size);
    }
    return 0;
}

/*
 * “失效”功能运行在设备定时器之外；它设置一个标志来模拟介质的移除。
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)) && !defined(timer_setup)
void sbull_invalidate(unsigned long ldev)
{
    struct sbull_dev *dev = (struct sbull_dev *)ldev;
#else
void sbull_invalidate(struct timer_list *ldev)
{
    struct sbull_dev *dev = from_timer(dev, ldev, timer);
#endif

    spin_lock(&dev->lock);
    if (dev->users || !dev->data) 
        printk(KERN_WARNING "sbull: 定时器完整性检查失败\n");
    else
        dev->media_change = 1;
    spin_unlock(&dev->lock);
}

/*
 * ioctl() 实现
 */
int sbull_ioctl(struct block_device *bdev, fmode_t mode,
                unsigned int cmd, unsigned long arg)
{
    long size;
    struct hd_geometry geo;
    struct sbull_dev *dev = bdev->bd_disk->private_data;

    switch (cmd) {
        case HDIO_GETGEO:
            /*
         * 获取几何信息：由于我们是虚拟设备，我们必须编造一些合理的东西。所以我们声称有 16 个扇区，四个磁头，并计算相应的柱面数。我们将数据起始位置设置在扇区四。
         */
        size = dev->size * (hardsect_size / KERNEL_SECTOR_SIZE);
        geo.cylinders = (size & ~0x3f) >> 6;
        geo.heads = 4;
        geo.sectors = 16;
        geo.start = 4;
        if (copy_to_user((void __user *)arg, &geo, sizeof(geo)))
            return -EFAULT;
        return 0;
    }

    return -ENOTTY; /* 未知命令 */
}

/*
 * 设备操作结构体。
 */
static struct block_device_operations sbull_ops = {
    .owner           = THIS_MODULE,
    .open 	         = sbull_open,
    .release 	 = sbull_release,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
    .media_changed   = sbull_media_changed,  // 在 v5.9 中废弃
#else
    .submit_bio      = sbull_make_request,
#endif
    .revalidate_disk = sbull_revalidate,
    .ioctl	         = sbull_ioctl
};

static struct blk_mq_ops mq_ops_simple = {
    .queue_rq = sbull_request,
};

static struct blk_mq_ops mq_ops_full = {
    .queue_rq = sbull_full_request,
};

/*
 * 设置我们内部的设备。
 */
static void setup_device(struct sbull_dev *dev, int which)
{
    /*
     * 获取一些内存。
     */
    memset(dev, 0, sizeof(struct sbull_dev));
    dev->size = nsectors * hardsect_size;
    dev->data = vmalloc(dev->size);
    if (dev->data == NULL) {
        printk(KERN_NOTICE "vmalloc 失败。\n");
        return;
    }
    spin_lock_init(&dev->lock);
    
    /*
     * 定时器，“使设备失效”。
     */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)) && !defined(timer_setup)
    init_timer(&dev->timer);
    dev->timer.data = (unsigned long)dev;
    dev->timer.function = sbull_invalidate;
#else
    timer_setup(&dev->timer, sbull_invalidate, 0);
#endif

    /*
     * I/O 队列，取决于我们是否使用自己的 make_request 函数。
     */
    switch (request_mode) {
        case RM_NOQUEUE:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0))
        dev->queue =  blk_generic_alloc_queue(sbull_make_request, NUMA_NO_NODE);
#else
        dev->queue =  blk_generic_alloc_queue(NUMA_NO_NODE);
#endif
        if (dev->queue == NULL)
            goto out_vfree;
        break;

        case RM_FULL:
        dev->queue = blk_mq_init_sq_queue(&dev->tag_set, &mq_ops_full, 128, BLK_MQ_F_SHOULD_MERGE);
        if (dev->queue == NULL)
            goto out_vfree;
        break;

        default:
        printk(KERN_NOTICE "错误的请求模式 %d，使用简单模式\n", request_mode);
            /* 继续... */
    
        case RM_SIMPLE:
        dev->queue = blk_mq_init_sq_queue(&dev->tag_set, &mq_ops_simple, 128, BLK_MQ_F_SHOULD_MERGE);
        if (dev->queue == NULL)
            goto out_vfree;
        break;
    }
    blk_queue_logical_block_size(dev->queue, hardsect_size);
    dev->queue->queuedata = dev;
    /*
     * 以及 gendisk 结构体。
     */
    dev->gd = alloc_disk(SBULL_MINORS);
    if (!dev->gd) {
        printk(KERN_NOTICE "alloc_disk 失败\n");
        goto out_vfree;
    }
    dev->gd->major = sbull_major;
    dev->gd->first_minor = which * SBULL_MINORS;
    dev->gd->fops = &sbull_ops;
    dev->gd->queue = dev->queue;
    dev->gd->private_data = dev;
    snprintf(dev->gd->disk_name, 32, "sbull%c", which + 'a');
    set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
    add_disk(dev->gd);
    return;

  out_vfree:
    if (dev->data)
        vfree(dev->data);
}

static int __init sbull_init(void)
{
    int i;
    /*
     * 注册。
     */
    sbull_major = register_blkdev(sbull_major, "sbull");
    if (sbull_major <= 0) {
        printk(KERN_WARNING "sbull: 无法获取主设备号\n");
        return -EBUSY;
    }
    /*
     * 分配设备数组，并初始化每个设备。
     */
    Devices = kmalloc(ndevices * sizeof(struct sbull_dev), GFP_KERNEL);
    if (Devices == NULL)
        goto out_unregister;
    for (i = 0; i < ndevices; i++) 
        setup_device(Devices + i, i);
    
    return 0;

  out_unregister:
    unregister_blkdev(sbull_major, "sbd");
    return -ENOMEM;
}

static void sbull_exit(void)
{
    int i;

    for (i = 0; i < ndevices; i++) {
        struct sbull_dev *dev = Devices + i;

        del_timer_sync(&dev->timer);
        if (dev->gd) {
            del_gendisk(dev->gd);
            put_disk(dev->gd);
        }
        if (dev->queue) {
            if (request_mode == RM_NOQUEUE)
                blk_put_queue(dev->queue);
            else
                blk_cleanup_queue(dev->queue);
        }
        if (dev->data)
            vfree(dev->data);
    }
    unregister_blkdev(sbull_major, "sbull");
    kfree(Devices);
}
    
module_init(sbull_init);
module_exit(sbull_exit);
