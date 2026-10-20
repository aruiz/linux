// SPDX-License-Identifier: GPL-2.0
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/eml_efi.h>

#define EML_BLK_MAX_DEVS 16

struct eml_blk_dev {
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	struct eml_block_io *bio;
	u32 media_id;
	u32 block_size;
	u64 num_blocks;
};

static struct eml_blk_dev eml_blk_devs[EML_BLK_MAX_DEVS];
static int eml_blk_count;
static int eml_blk_major;

static blk_status_t eml_blk_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct eml_blk_dev *dev = rq->q->queuedata;
	struct bio_vec bvec;
	struct req_iterator iter;
	u64 lba = blk_rq_pos(rq) * 512 / dev->block_size;
	efi_status_t status;
	bool is_write = rq_data_dir(rq) == WRITE;

	blk_mq_start_request(rq);

	rq_for_each_segment(bvec, rq, iter) {
		void *buf = page_address(bvec.bv_page) + bvec.bv_offset;
		unsigned long size = bvec.bv_len;

		if (is_write)
			status = dev->bio->write_blocks(dev->bio, dev->media_id,
							lba, size, buf);
		else
			status = dev->bio->read_blocks(dev->bio, dev->media_id,
						       lba, size, buf);

		if (status != EFI_SUCCESS) {
			blk_mq_end_request(rq, BLK_STS_IOERR);
			return BLK_STS_OK;
		}

		lba += size / dev->block_size;
	}

	blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops eml_blk_mq_ops = {
	.queue_rq = eml_blk_queue_rq,
};

static const struct block_device_operations eml_blk_fops = {
	.owner = THIS_MODULE,
};

static int __init eml_blk_init(void)
{
	efi_guid_t bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
	efi_handle_t *handles = NULL;
	unsigned long num_handles = 0;
	efi_status_t status;
	unsigned long i;
	int ret;

	eml_blk_major = register_blkdev(0, "efiblk");
	if (eml_blk_major < 0) {
		pr_err("EML: register_blkdev failed: %d\n", eml_blk_major);
		return eml_blk_major;
	}

	status = eml_systab->boot_services->locate_handle_buffer(
		EFI_LOCATE_BY_PROTOCOL, &bio_guid, NULL, &num_handles,
		&handles);

	if (status != EFI_SUCCESS || num_handles == 0) {
		pr_info("EML: no EFI Block I/O devices found\n");
		return 0;
	}

	pr_info("EML: found %lu EFI Block I/O device(s)\n", num_handles);

	for (i = 0; i < num_handles && eml_blk_count < EML_BLK_MAX_DEVS; i++) {
		struct eml_block_io *bio;
		struct eml_blk_dev *dev;
		struct gendisk *disk;

		status = eml_systab->boot_services->handle_protocol(
			handles[i], &bio_guid, (void **)&bio);
		if (status != EFI_SUCCESS)
			continue;

		if (!bio->media || !bio->media->media_present)
			continue;

		if (bio->media->logical_partition)
			continue;

		dev = &eml_blk_devs[eml_blk_count];
		dev->bio = bio;
		dev->media_id = bio->media->media_id;
		dev->block_size = bio->media->block_size;
		dev->num_blocks = bio->media->last_block + 1;

		memset(&dev->tag_set, 0, sizeof(dev->tag_set));
		dev->tag_set.ops = &eml_blk_mq_ops;
		dev->tag_set.nr_hw_queues = 1;
		dev->tag_set.queue_depth = 1;
		dev->tag_set.numa_node = NUMA_NO_NODE;
		ret = blk_mq_alloc_tag_set(&dev->tag_set);
		if (ret) {
			pr_err("EML: blk_mq_alloc_tag_set failed: %d\n", ret);
			continue;
		}

		{
			struct queue_limits lim = {
				.logical_block_size = dev->block_size,
				.physical_block_size = dev->block_size,
			};

			disk = blk_mq_alloc_disk(&dev->tag_set, &lim, NULL);
		}
		if (IS_ERR(disk)) {
			pr_err("EML: blk_mq_alloc_disk failed\n");
			blk_mq_free_tag_set(&dev->tag_set);
			continue;
		}

		dev->disk = disk;
		disk->major = eml_blk_major;
		disk->first_minor = eml_blk_count * 16;
		disk->minors = 16;
		disk->fops = &eml_blk_fops;
		disk->queue->queuedata = dev;
		snprintf(disk->disk_name, sizeof(disk->disk_name), "efiblk%d",
			 eml_blk_count);

		set_capacity(disk, dev->num_blocks * (dev->block_size / 512));

		ret = add_disk(disk);
		if (ret) {
			pr_err("EML: add_disk failed: %d\n", ret);
			put_disk(disk);
			blk_mq_free_tag_set(&dev->tag_set);
			continue;
		}

		pr_info("EML: efiblk%d: %llu blocks * %u bytes = %llu MB%s\n",
			eml_blk_count, dev->num_blocks, dev->block_size,
			dev->num_blocks * dev->block_size / (1024 * 1024),
			bio->media->read_only ? " (read-only)" : "");

		eml_blk_count++;
	}

	if (handles)
		eml_systab->boot_services->free_pool(handles);

	return 0;
}
device_initcall(eml_blk_init);
