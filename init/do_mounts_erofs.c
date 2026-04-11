// SPDX-License-Identifier: GPL-2.0
/*
 * Support for erofs-formatted initrd images: scan for segments, mount
 * erofs via a read-only RAM block device, overlay for writable root.
 */
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"
#include "do_mounts_erofs_internal.h"

bool initrd_is_erofs;
static struct initrd_segment initrd_segs[INITRD_MAX_SEGMENTS];
static int initrd_seg_count;

/*
 * Walk concatenated uncompressed newc cpio archives and return the byte
 * offset past the last entry.  Returns 0 if the buffer is not a cpio.
 */
unsigned long __init skip_cpio_prefix(void *data, unsigned long len)
{
	unsigned long offset = 0;
	bool found = false;

	while (offset + CPIO_HDRLEN <= len) {
		char *entry = data + offset;
		unsigned long namesize, filesize, next;
		u64 next64;

		if (!is_cpio_newc_hdr(entry)) {
			if (!found)
				return 0;
			break;
		}
		found = true;

		namesize = simple_strntoul(&entry[CPIO_OFF_NAMESIZE], NULL, 16,
					   CPIO_FIELD_LEN);
		filesize = simple_strntoul(&entry[CPIO_OFF_FILESIZE], NULL, 16,
					   CPIO_FIELD_LEN);

		next64 = ALIGN((u64)offset + CPIO_HDRLEN + namesize, 4);
		next64 = ALIGN(next64 + filesize, 4);
		if (next64 > len)
			break;
		next = (unsigned long)next64;

		if (namesize >= sizeof(CPIO_TRAILER) &&
		    memcmp(&entry[CPIO_HDRLEN], CPIO_TRAILER,
			   sizeof(CPIO_TRAILER) - 1) == 0) {
			unsigned long saved = next;
			unsigned long pos = next;

			while (pos < len && !*(char *)(data + pos))
				pos++;

			if (pos + CPIO_HDRLEN <= len &&
			    is_cpio_newc_hdr(data + pos)) {
				offset = pos;
				continue;
			}
			return saved;
		}
		offset = next;
	}
	return found ? offset : 0;
}

/*
 * Parse one EROFS image at @buf + @offset.  Returns the image size on
 * success or 0 on failure.
 */
unsigned long __init try_parse_erofs(void *buf, unsigned long offset,
				     unsigned long len)
{
	struct erofs_super_block *sb;
	u64 blocks, image_size64;
	u8 blkszbits;

	if (offset + EROFS_SUPER_OFFSET + sizeof(*sb) > len)
		return 0;

	sb = buf + offset + EROFS_SUPER_OFFSET;
	if (le32_to_cpu(sb->magic) != EROFS_SUPER_MAGIC_V1)
		return 0;

	blkszbits = sb->blkszbits;
	if (blkszbits < 9 || blkszbits > 30)
		return 0;

	blocks = le32_to_cpu(sb->blocks_lo);
	if (le32_to_cpu(sb->feature_incompat) & EROFS_FEATURE_INCOMPAT_48BIT)
		blocks |= (u64)le16_to_cpu(sb->rb.blocks_hi) << 32;

	if (!blocks || blocks > (U64_MAX >> blkszbits))
		return 0;

	image_size64 = blocks << blkszbits;
	if (image_size64 != (unsigned long)image_size64)
		return 0;
	if (image_size64 > len - offset)
		return 0;

	return (unsigned long)image_size64;
}

/*
 * Cheap detection: scan for the EROFS superblock magic without parsing
 * any cpio headers.  Full segment discovery is deferred to mount time.
 */
bool __init initrd_has_erofs(void *buf, unsigned long len)
{
	unsigned long off;

	for (off = 0; off + EROFS_SUPER_OFFSET + sizeof(__le32) <= len; off++) {
		__le32 *magic = buf + off + EROFS_SUPER_OFFSET;

		if (le32_to_cpu(*magic) == EROFS_SUPER_MAGIC_V1 &&
		    try_parse_erofs(buf, off, len) > 0) {
			initrd_is_erofs = true;
			pr_info("initrd: EROFS image detected at offset %lu\n",
				off);
			return true;
		}
	}
	return false;
}

/*
 * Single-pass scan-and-mount: walk the initrd left-to-right, discover
 * each segment, and mount EROFS images immediately as they are found.
 * Populates initrd_segs[] and initrd_seg_count for later use.
 */
static int __init erofs_initrd_mount_layers(void)
{
	void *buf = (void *)initrd_start;
	unsigned long len = initrd_end - initrd_start;
	unsigned long offset = 0;
	int ret;

	init_mkdir("/initrd_layers", 0755);

	while (offset < len && initrd_seg_count < INITRD_MAX_SEGMENTS) {
		unsigned long cpio_len, erofs_size;
		int idx = initrd_seg_count;

		cpio_len = skip_cpio_prefix(buf + offset, len - offset);
		if (cpio_len > 0) {
			initrd_segs[idx].type = INITRD_SEG_CPIO;
			initrd_segs[idx].offset = offset;
			initrd_segs[idx].size = cpio_len;
			initrd_seg_count++;
			offset += cpio_len;
			continue;
		}

		erofs_size = try_parse_erofs(buf, offset, len);
		if (erofs_size > 0) {
			char mntpoint[32], devname[32], devpath[40];
			dev_t dev;

			initrd_segs[idx].type = INITRD_SEG_EROFS;
			initrd_segs[idx].offset = offset;
			initrd_segs[idx].size = erofs_size;
			initrd_seg_count++;

			snprintf(mntpoint, sizeof(mntpoint),
				 "/initrd_layers/%d", idx);
			init_mkdir(mntpoint, 0755);

			snprintf(devname, sizeof(devname), "initrd%d", idx);
			dev = initrd_blkdev_create(buf + offset, erofs_size,
						   devname);
			if (!dev) {
				pr_err("initrd: failed to create blkdev for segment %d\n",
				       idx);
				return -ENOMEM;
			}

			snprintf(devpath, sizeof(devpath), "/dev/%s", devname);
			create_dev(devpath, dev);
			ret = init_mount(devpath, mntpoint, "erofs",
					 MS_RDONLY, NULL);
			if (ret) {
				pr_err("initrd: failed to mount erofs segment %d: %d\n",
				       idx, ret);
				return ret;
			}
			offset += erofs_size;
			continue;
		}

		if (!((char *)buf)[offset]) {
			offset++;
			continue;
		}

		/* Unrecognized trailing data — treat as compressed cpio. */
		initrd_segs[idx].type = INITRD_SEG_CPIO;
		initrd_segs[idx].offset = offset;
		initrd_segs[idx].size = len - offset;
		initrd_seg_count++;
		break;
	}

	if (initrd_seg_count <= 0) {
		pr_err("initrd: no valid segments found\n");
		return -EINVAL;
	}
	return 0;
}

static int __init erofs_initrd_setup_overlay(void)
{
	char *opts;
	int len, ret, i;

	init_mkdir("/root", 0755);
	init_mkdir("/initrd_layers/rw", 0755);

	ret = init_mount("tmpfs", "/initrd_layers/rw", "tmpfs", 0, NULL);
	if (ret) {
		pr_err("initrd: failed to mount tmpfs: %d\n", ret);
		return ret;
	}
	init_mkdir("/initrd_layers/rw/upper", 0755);
	init_mkdir("/initrd_layers/rw/work", 0755);

	opts = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	len = snprintf(opts, PAGE_SIZE,
		       "upperdir=/initrd_layers/rw/upper,"
		       "workdir=/initrd_layers/rw/work,"
		       "lowerdir=");
	if (len >= PAGE_SIZE) {
		kfree(opts);
		pr_err("initrd: overlayfs opts too long\n");
		return -ENAMETOOLONG;
	}
	for (i = initrd_seg_count - 1; i >= 0; i--) {
		if (initrd_segs[i].type != INITRD_SEG_EROFS)
			continue;
		len += snprintf(opts + len, PAGE_SIZE - len,
				"/initrd_layers/%d%s", i, i > 0 ? ":" : "");
		if (len >= PAGE_SIZE) {
			kfree(opts);
			pr_err("initrd: overlayfs opts too long\n");
			return -ENAMETOOLONG;
		}
	}

	ret = init_mount("overlay", "/root", "overlay", 0, opts);
	kfree(opts);
	if (ret) {
		pr_err("initrd: failed to mount overlayfs: %d\n", ret);
		return ret;
	}
	pr_info("initrd: mounted writable overlayfs with %d lower layer(s) at /root\n",
		initrd_seg_count);
	return 0;
}

/*
 * Free pages belonging to CPIO-only regions and register EROFS page
 * ranges for deferred freeing when the block devices are destroyed.
 */
static void __init erofs_initrd_free_cpio_pages(void)
{
	unsigned long base = initrd_start;
	unsigned long total_end = PAGE_ALIGN(initrd_end);
	unsigned long free_start = base;
	unsigned long last_erofs_pe = base;
	int i;

	if (do_retain_initrd)
		return;

	for (i = 0; i < initrd_seg_count; i++) {
		unsigned long seg_start, ps, pe;

		if (initrd_segs[i].type != INITRD_SEG_EROFS)
			continue;

		seg_start = base + initrd_segs[i].offset;
		ps = PAGE_ALIGN_DOWN(seg_start);
		pe = PAGE_ALIGN(seg_start + initrd_segs[i].size);

		if (ps < last_erofs_pe)
			ps = last_erofs_pe;

		if (free_start < ps)
			free_initrd_mem(free_start, ps);

		initrd_blkdev_add_pages(ps, pe);
		last_erofs_pe = pe;
		free_start = pe;
	}

	if (free_start < total_end)
		free_initrd_mem(free_start, total_end);
}

/*
 * Tear down the namespace-visible mounts under /initrd_layers.  Overlayfs
 * holds independent private mount clones, so removing these from the mount
 * namespace is safe and keeps /proc/mounts clean.
 */
static void __init erofs_initrd_cleanup_mounts(void)
{
	int i;

	for (i = initrd_seg_count - 1; i >= 0; i--) {
		char mntpoint[32];

		if (initrd_segs[i].type != INITRD_SEG_EROFS)
			continue;

		snprintf(mntpoint, sizeof(mntpoint), "/initrd_layers/%d", i);
		init_umount(mntpoint, 0);

		{
			char devpath[40];

			snprintf(devpath, sizeof(devpath), "/dev/initrd%d", i);
			init_unlink(devpath);
		}
		init_rmdir(mntpoint);
	}

	init_umount("/initrd_layers/rw", 0);
	init_rmdir("/initrd_layers/rw");
	init_rmdir("/initrd_layers");
}

int __init erofs_initrd_setup(void)
{
	int ret;

	ret = erofs_initrd_mount_layers();
	if (ret)
		goto err_cleanup;

	erofs_initrd_free_cpio_pages();
	initrd_start = 0;
	initrd_end = 0;

	ret = erofs_initrd_setup_overlay();
	if (ret)
		goto err_cleanup;

	erofs_initrd_cleanup_mounts();

	pr_info("initrd: root filesystem ready at /root\n");
	return 0;

err_cleanup:
	erofs_initrd_cleanup_mounts();
	return ret;
}
