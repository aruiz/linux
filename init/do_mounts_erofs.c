// SPDX-License-Identifier: GPL-2.0
/*
 * Mixed cpio/erofs initrd: scan for segments, mount erofs via mem-backed
 * mode, extract cpio into tmpfs, assemble with overlayfs for writable root.
 */
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/page-flags.h>
#include <linux/memblock.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"
#include "do_mounts_erofs_internal.h"

extern void __init erofs_set_mem_region(unsigned long addr, unsigned long size);
extern char *unpack_to_rootfs(char *buf, unsigned long len);

static bool initrd_is_erofs;
static struct initrd_segment initrd_segs[INITRD_MAX_SEGMENTS];
static int initrd_seg_count;

static unsigned long __init parse_cpio_hex(const char *s, int count)
{
	unsigned long val = 0;

	while (count--) {
		char c = *s++;

		val <<= 4;
		if (c >= '0' && c <= '9')
			val += c - '0';
		else if (c >= 'a' && c <= 'f')
			val += c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			val += c - 'A' + 10;
	}
	return val;
}

/*
 * Walk concatenated uncompressed newc cpio archives and return the byte
 * offset past the last entry.  Returns 0 if the buffer is not a cpio.
 */
unsigned long __init skip_cpio_prefix(void *data, unsigned long len)
{
	unsigned long offset = 0;
	bool found = false;

	while (offset + CPIO_HDRLEN <= len) {
		char *hdr = data + offset;
		unsigned long namesize, filesize, next;
		u64 next64;

		if (memcmp(hdr, "070701", 6) != 0 &&
		    memcmp(hdr, "070702", 6) != 0) {
			if (!found)
				return 0;
			break;
		}
		found = true;

		namesize = parse_cpio_hex(hdr + 94, 8);
		filesize = parse_cpio_hex(hdr + 54, 8);

		next64 = ALIGN((u64)offset + CPIO_HDRLEN + namesize, 4);
		next64 = ALIGN(next64 + filesize, 4);
		if (next64 > len)
			break;
		next = (unsigned long)next64;

		if (namesize >= 11 &&
		    memcmp(hdr + CPIO_HDRLEN, "TRAILER!!!", 10) == 0) {
			unsigned long saved = next;
			unsigned long pos = next;

			while (pos < len && !*(char *)(data + pos))
				pos++;

			if (pos + CPIO_HDRLEN <= len &&
			    (memcmp(data + pos, "070701", 6) == 0 ||
			     memcmp(data + pos, "070702", 6) == 0)) {
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
static unsigned long __init try_parse_erofs(void *buf, unsigned long offset,
					    unsigned long len)
{
	struct erofs_initrd_sb *sb;
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
		blocks |= (u64)le16_to_cpu(sb->rb_field) << 32;

	image_size64 = blocks << blkszbits;
	if (!image_size64 || image_size64 > len - offset)
		return 0;

	return (unsigned long)image_size64;
}

/*
 * Scan forward from @start looking for the beginning of the next
 * identifiable segment (uncompressed cpio or EROFS).  Returns @len
 * if nothing is found.
 */
static unsigned long __init find_next_segment_start(void *buf,
						    unsigned long start,
						    unsigned long len)
{
	unsigned long pos;

	for (pos = start; pos < len; pos++) {
		if (pos + 6 <= len &&
		    (memcmp(buf + pos, "070701", 6) == 0 ||
		     memcmp(buf + pos, "070702", 6) == 0))
			return pos;

		if (try_parse_erofs(buf, pos, len) > 0)
			return pos;
	}
	return len;
}

/*
 * Scan initrd left-to-right for cpio and erofs segments.  Unrecognized
 * non-NUL data (e.g. compressed cpio) extends to the next identifiable
 * segment boundary.
 */
int __init initrd_scan_segments(void *buf, unsigned long len,
				struct initrd_segment *segs, int max_segs,
				bool *has_erofs)
{
	unsigned long offset = 0;
	int count = 0;
	bool found_erofs = false;

	while (offset < len && count < max_segs) {
		unsigned long cpio_len, erofs_size;

		cpio_len = skip_cpio_prefix(buf + offset, len - offset);
		if (cpio_len > 0) {
			segs[count].type = INITRD_SEG_CPIO;
			segs[count].offset = offset;
			segs[count].size = cpio_len;
			count++;
			offset += cpio_len;
			continue;
		}

		erofs_size = try_parse_erofs(buf, offset, len);
		if (erofs_size > 0) {
			segs[count].type = INITRD_SEG_EROFS;
			segs[count].offset = offset;
			segs[count].size = erofs_size;
			count++;
			offset += erofs_size;
			found_erofs = true;
			continue;
		}

		/* NUL padding — advance one byte to avoid overshooting. */
		if (!((char *)buf)[offset]) {
			offset++;
			continue;
		}

		/* Unrecognized data (e.g. compressed cpio) — find next boundary. */
		{
			unsigned long seg_end =
				find_next_segment_start(buf, offset + 1, len);

			segs[count].type = INITRD_SEG_CPIO;
			segs[count].offset = offset;
			segs[count].size = seg_end - offset;
			count++;
			offset = seg_end;
			continue;
		}
	}

	if (has_erofs)
		*has_erofs = found_erofs;
	return count;
}

bool __init initrd_has_erofs(void *buf, unsigned long len)
{
	bool found_erofs = false;
	int i;

	initrd_seg_count = initrd_scan_segments(
		buf, len, initrd_segs, INITRD_MAX_SEGMENTS, &found_erofs);

	if (found_erofs) {
		initrd_is_erofs = true;
		for (i = 0; i < initrd_seg_count; i++)
			pr_info("initrd: segment %d: %s at offset %lu, size %lu\n",
				i,
				initrd_segs[i].type == INITRD_SEG_EROFS ?
					"erofs" :
					"cpio",
				initrd_segs[i].offset, initrd_segs[i].size);
	}
	return found_erofs;
}

bool __init erofs_initrd_is_active(void)
{
	return initrd_is_erofs;
}

static int __init unpack_cpio_to(const char *mountpoint, void *data,
				 unsigned long len)
{
	struct path saved_root, saved_pwd, new_root;
	char *err;
	int ret;

	get_fs_root(current->fs, &saved_root);
	get_fs_pwd(current->fs, &saved_pwd);

	ret = kern_path(mountpoint, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&new_root);
	if (ret)
		goto out;

	set_fs_root(current->fs, &new_root);
	set_fs_pwd(current->fs, &new_root);
	path_put(&new_root);

	err = unpack_to_rootfs(data, len);
	ret = err ? -EIO : 0;
	if (err)
		pr_err("initrd: cpio extraction to %s failed: %s\n", mountpoint,
		       err);

	set_fs_root(current->fs, &saved_root);
	set_fs_pwd(current->fs, &saved_pwd);
out:
	path_put(&saved_root);
	path_put(&saved_pwd);
	return ret;
}


static int __init erofs_initrd_mount_layers(void)
{
	int i, ret;

	init_mkdir("/initrd_layers", 0755);

	for (i = 0; i < initrd_seg_count; i++) {
		char mntpoint[32];

		snprintf(mntpoint, sizeof(mntpoint), "/initrd_layers/%d", i);
		init_mkdir(mntpoint, 0755);

		if (initrd_segs[i].type == INITRD_SEG_EROFS) {
			unsigned long addr =
				initrd_start + initrd_segs[i].offset;

			erofs_set_mem_region(addr, initrd_segs[i].size);

			ret = init_mount("none", mntpoint, "erofs", MS_RDONLY,
					 NULL);
			if (ret) {
				pr_err("initrd: failed to mount erofs segment %d on %s: %d\n",
				       i, mntpoint, ret);
				return ret;
			}
		} else {
			ret = init_mount("tmpfs", mntpoint, "tmpfs", 0, NULL);
			if (ret) {
				pr_err("initrd: failed to mount tmpfs for cpio segment %d: %d\n",
				       i, ret);
				return ret;
			}

			ret = unpack_cpio_to(
				mntpoint,
				(void *)(initrd_start + initrd_segs[i].offset),
				initrd_segs[i].size);
			if (ret) {
				pr_err("initrd: failed to extract cpio segment %d: %d\n",
				       i, ret);
				return ret;
			}
		}
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
	for (i = initrd_seg_count - 1; i >= 0; i--) {
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
 * Hand EROFS segment pages to the erofs page cache and free CPIO pages.
 * Boundary pages are assigned to EROFS by rounding outward to page size.
 */
static void __init erofs_initrd_prep_pages(void)
{
	unsigned long base = initrd_start;
	unsigned long total_end = PAGE_ALIGN(initrd_end);
	unsigned long free_start = base;
	unsigned long last_erofs_pe = base;
	unsigned long addr;
	int i;

	for (i = 0; i < initrd_seg_count; i++) {
		unsigned long seg_start, ps, pe;

		if (initrd_segs[i].type != INITRD_SEG_EROFS)
			continue;

		seg_start = base + initrd_segs[i].offset;
		ps = PAGE_ALIGN_DOWN(seg_start);
		pe = PAGE_ALIGN(seg_start + initrd_segs[i].size);

		if (ps < last_erofs_pe)
			ps = last_erofs_pe;

		if (free_start < ps && !do_retain_initrd)
			free_initrd_mem(free_start, ps);

		for (addr = ps; addr < pe; addr += PAGE_SIZE) {
			struct page *page = virt_to_page(addr);

			ClearPageReserved(page);
			adjust_managed_page_count(page, 1);
			put_page(page);
		}

		if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK) && !do_retain_initrd)
			memblock_free((void *)ps, pe - ps);

		last_erofs_pe = pe;
		free_start = pe;
	}

	if (free_start < total_end && !do_retain_initrd)
		free_initrd_mem(free_start, total_end);
}

int __init erofs_initrd_setup(void)
{
	int ret;

	if (initrd_seg_count <= 0) {
		pr_err("initrd: no valid segments found\n");
		return -EINVAL;
	}

	ret = erofs_initrd_mount_layers();
	if (ret)
		return ret;

	erofs_initrd_prep_pages();
	initrd_start = 0;
	initrd_end = 0;

	ret = erofs_initrd_setup_overlay();
	if (ret)
		return ret;

	pr_info("initrd: root filesystem ready at /root\n");
	return 0;
}
