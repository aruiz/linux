// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only block device backed by a contiguous kernel memory region.
 * Used during early boot to expose initrd EROFS segments to the standard
 * block-device mount path, avoiding custom EROFS page-cache machinery.
 *
 * Lifecycle:
 *   1. initrd_blkdev_create()    — creates and registers device during __init
 *   2. initrd_blkdev_add_pages() — records page ranges for deferred freeing
 *   3. initrd_blkdev_shutdown()  — called after the overlay is assembled (or
 *      on the error path).  del_gendisk prevents new opens; put_disk drops
 *      our explicit reference.  Existing EROFS mounts keep the gendisks
 *      alive until switch_root unmounts the overlay.
 *   4. When the final gendisk refcount drops, .free_disk returns the backing
 *      pages to the buddy allocator via free_reserved_page().
 */
#include <linux/blkdev.h>
#include <linux/crash_reserve.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>

#define INITRD_BLKDEV_MAX 32

struct initrd_blkdev {
	void *data;
	unsigned long size;
	struct gendisk *disk;
};

static int initrd_blk_major;
static int initrd_blk_count;
static struct gendisk *initrd_blk_disks[INITRD_BLKDEV_MAX];
static atomic_t initrd_blk_active = ATOMIC_INIT(0);

struct initrd_blk_page_range {
	unsigned long start;
	unsigned long end;
};

static struct initrd_blk_page_range initrd_blk_pages[INITRD_BLKDEV_MAX];
static int initrd_blk_page_count;

static bool initrd_blk_page_in_crashk(unsigned long addr)
{
#ifdef CONFIG_CRASH_RESERVE
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end = (unsigned long)__va(crashk_res.end + 1);

	return crashk_start != crashk_end && addr >= crashk_start &&
	       addr < crashk_end;
#else
	return false;
#endif
}

static void initrd_blk_free_pages(void)
{
	int i;

	for (i = 0; i < initrd_blk_page_count; i++) {
		unsigned long addr;

		for (addr = initrd_blk_pages[i].start;
		     addr < initrd_blk_pages[i].end; addr += PAGE_SIZE) {
			/*
			 * Pages that overlap the crashkernel reserved region
			 * must not be returned to the buddy allocator.  Zero
			 * them to avoid leaking initrd data into crash dumps.
			 */
			if (initrd_blk_page_in_crashk(addr)) {
				memset((void *)addr, 0, PAGE_SIZE);
				continue;
			}
			free_reserved_page(virt_to_page(addr));
		}
	}
	initrd_blk_page_count = 0;
}

static void initrd_blk_free_disk(struct gendisk *disk)
{
	struct initrd_blkdev *dev = disk->private_data;

	kfree(dev);
	if (atomic_dec_and_test(&initrd_blk_active))
		initrd_blk_free_pages();
}

void __init initrd_blkdev_shutdown(void)
{
	int i;

	for (i = 0; i < initrd_blk_count; i++) {
		if (initrd_blk_disks[i]) {
			del_gendisk(initrd_blk_disks[i]);
			put_disk(initrd_blk_disks[i]);
			initrd_blk_disks[i] = NULL;
		}
	}
}

static void initrd_blk_submit_bio(struct bio *bio)
{
	struct initrd_blkdev *dev = bio->bi_bdev->bd_disk->private_data;
	struct bio_vec bv;
	struct bvec_iter iter;
	loff_t pos = (loff_t)bio->bi_iter.bi_sector << SECTOR_SHIFT;

	if (op_is_write(bio->bi_opf)) {
		bio_io_error(bio);
		return;
	}

	bio_for_each_segment(bv, bio, iter) {
		void *dst = bvec_kmap_local(&bv);
		unsigned long avail =
			clamp_t(loff_t, dev->size - pos, 0, bv.bv_len);

		if (avail)
			memcpy(dst, dev->data + pos, avail);
		if (avail < bv.bv_len)
			memset(dst + avail, 0, bv.bv_len - avail);
		kunmap_local(dst);
		pos += bv.bv_len;
	}
	bio_endio(bio);
}

static const struct block_device_operations initrd_blk_fops = {
	.owner = THIS_MODULE,
	.submit_bio = initrd_blk_submit_bio,
	.free_disk = initrd_blk_free_disk,
};

/*
 * Record a page-aligned virtual address range to be returned to the
 * buddy allocator when the last initrd block device is destroyed.
 */
void __init initrd_blkdev_add_pages(unsigned long start, unsigned long end)
{
	if (WARN_ON_ONCE(!PAGE_ALIGNED(start) || !PAGE_ALIGNED(end)))
		return;
	if (WARN_ON_ONCE(initrd_blk_page_count >= INITRD_BLKDEV_MAX))
		return;
	initrd_blk_pages[initrd_blk_page_count].start = start;
	initrd_blk_pages[initrd_blk_page_count].end = end;
	initrd_blk_page_count++;
}

/*
 * Create a read-only block device named @name backed by @size bytes
 * starting at @data.  Returns the dev_t on success, 0 on failure.
 * The memory region must remain valid for the lifetime of the device.
 */
dev_t __init initrd_blkdev_create(void *data, unsigned long size,
				  const char *name)
{
	struct queue_limits lim = {
		.physical_block_size = PAGE_SIZE,
		.logical_block_size = SECTOR_SIZE,
		.features = BLK_FEAT_SYNCHRONOUS,
	};
	struct initrd_blkdev *dev;
	struct gendisk *disk;
	int err;

	if (!initrd_blk_major) {
		initrd_blk_major = register_blkdev(0, "initrdblk");
		if (initrd_blk_major < 0)
			return 0;
	}

	if (WARN_ON_ONCE(initrd_blk_count >= INITRD_BLKDEV_MAX))
		return 0;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return 0;

	dev->data = data;
	dev->size = size;

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		kfree(dev);
		return 0;
	}

	disk->major = initrd_blk_major;
	disk->first_minor = initrd_blk_count;
	disk->minors = 1;
	disk->fops = &initrd_blk_fops;
	disk->private_data = dev;
	strscpy(disk->disk_name, name, DISK_NAME_LEN);
	set_capacity(disk, DIV_ROUND_UP(size, SECTOR_SIZE));
	set_disk_ro(disk, true);
	dev->disk = disk;

	err = add_disk(disk);
	if (err) {
		put_disk(disk);
		return 0;
	}

	initrd_blk_disks[initrd_blk_count] = disk;
	initrd_blk_count++;
	atomic_inc(&initrd_blk_active);

	return disk_devt(disk);
}
