// SPDX-License-Identifier: GPL-2.0
/*
 * Support for erofs-formatted initrd images: scan for segments, mount
 * erofs via a read-only RAM block device, overlay for writable root.
 */
#include <linux/init.h>
#include <linux/init_syscalls.h>
#include <linux/initrd.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"
#include "do_mounts_erofs_internal.h"

extern dev_t __init initrd_blkdev_create(void *data, unsigned long size,
					 const char *name);
extern void __init initrd_blkdev_add_pages(unsigned long start,
					   unsigned long end);

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

int __init erofs_initrd_setup(void)
{
	return -ENODEV;
}
