// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for initrd segment scanning and detection.
 *
 * Tests exercise the cpio prefix skipping logic, generic segment
 * scanner (cpio/erofs interleaving), and detection API that form the
 * basis of the mixed cpio/erofs initrd boot path.
 */
#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/magic.h>
#include "do_mounts.h"
#include "do_mounts_erofs_internal.h"

/*
 * Build a minimal newc cpio entry in @out.  Returns bytes written.
 * @namesize includes the NUL terminator.
 */
static size_t __init fill_cpio_entry(char *out, const char *magic,
				     const char *fname, unsigned int namesize,
				     unsigned int filesize, const char *data)
{
	size_t off;

	off = sprintf(out,
		      "%s"
		      "%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x"
		      "%s",
		      magic,
		      /* ino */ 1, /* mode */ 0100644, /* uid */ 0,
		      /* gid */ 0, /* nlink */ 1, /* mtime */ 0, filesize,
		      /* devmajor */ 0, /* devminor */ 1,
		      /* rdevmajor */ 0, /* rdevminor */ 0, namesize,
		      /* csum */ 0, fname) +
	      1;

	off = ALIGN(CPIO_HDRLEN + namesize, 4);
	if (data && filesize)
		memcpy(out + off, data, filesize);
	off = ALIGN(off + filesize, 4);
	return off;
}

static size_t __init fill_cpio_trailer(char *out)
{
	return fill_cpio_entry(out, CPIO_MAGIC_NEWC, CPIO_TRAILER,
			       sizeof(CPIO_TRAILER), 0, NULL);
}

/*
 * Place a minimal EROFS superblock at @buf.  The buffer must be at
 * least EROFS_SUPER_OFFSET + sizeof(struct erofs_super_block) bytes.
 * Returns the total image size implied by @blocks / @blkszbits.
 */
static unsigned long __init place_erofs_sb(void *buf, u8 blkszbits,
					   u32 blocks_lo, u16 blocks_hi,
					   bool use_48bit)
{
	struct erofs_super_block *sb = buf + EROFS_SUPER_OFFSET;

	memset(buf, 0, EROFS_SUPER_OFFSET + sizeof(*sb));
	sb->magic = cpu_to_le32(EROFS_SUPER_MAGIC_V1);
	sb->blkszbits = blkszbits;
	sb->blocks_lo = cpu_to_le32(blocks_lo);
	if (use_48bit) {
		sb->feature_incompat =
			cpu_to_le32(EROFS_FEATURE_INCOMPAT_48BIT);
		sb->rb.blocks_hi = cpu_to_le16(blocks_hi);
	}
	return (unsigned long)blocks_lo << blkszbits;
}

/* --- skip_cpio_prefix tests --- */

static void __init test_skip_cpio_no_cpio(struct kunit *test)
{
	char buf[256];

	memset(buf, 0, sizeof(buf));
	KUNIT_EXPECT_EQ(test, skip_cpio_prefix(buf, sizeof(buf)), 0UL);
}

static void __init test_skip_cpio_garbage(struct kunit *test)
{
	char buf[256];

	memset(buf, 'X', sizeof(buf));
	KUNIT_EXPECT_EQ(test, skip_cpio_prefix(buf, sizeof(buf)), 0UL);
}

static void __init test_skip_cpio_too_short(struct kunit *test)
{
	char buf[64];

	memcpy(buf, CPIO_MAGIC_NEWC, CPIO_MAGIC_LEN);
	KUNIT_EXPECT_EQ(test, skip_cpio_prefix(buf, sizeof(buf)), 0UL);
}

static void __init test_skip_cpio_single_with_trailer(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "testfile", 9, 4, "data");
	off += fill_cpio_trailer(buf + off);

	result = skip_cpio_prefix(buf, off + 128);
	KUNIT_EXPECT_GT(test, result, 0UL);
	KUNIT_EXPECT_LE(test, result, off + 128);

	kfree(buf);
}

static void __init test_skip_cpio_with_filedata(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;
	const char *payload = "hello world, this is test data";

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "bigfile", 8,
			      strlen(payload), payload);
	off += fill_cpio_trailer(buf + off);

	result = skip_cpio_prefix(buf, off);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)off);

	kfree(buf);
}

static void __init test_skip_cpio_070702_magic(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_CRC, "csumfile", 9, 0, NULL);
	off += fill_cpio_trailer(buf + off);

	result = skip_cpio_prefix(buf, off);
	KUNIT_EXPECT_GT(test, result, 0UL);

	kfree(buf);
}

static void __init test_skip_cpio_nul_padding_no_next(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "afile", 6, 0, NULL);
	off += fill_cpio_trailer(buf + off);
	memset(buf + off, 0, 512);

	result = skip_cpio_prefix(buf, off + 512);
	/*
	 * NUL padding after the last trailer is NOT consumed when no
	 * subsequent cpio follows — prevents eating into the leading
	 * zeros of a following EROFS image.
	 */
	KUNIT_EXPECT_EQ(test, result, (unsigned long)off);

	kfree(buf);
}

static void __init test_skip_cpio_no_trailer(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "notrailer", 10, 0, NULL);
	/* no trailer, followed by non-cpio data */
	memset(buf + off, 'Z', 256);

	result = skip_cpio_prefix(buf, off + 256);
	/* should still return the offset past the cpio entry */
	KUNIT_EXPECT_EQ(test, result, (unsigned long)off);

	kfree(buf);
}

/*
 * Two concatenated cpio archives: the scanner should walk past both.
 */
static void __init test_skip_cpio_concatenated(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "file1", 6, 3, "abc");
	off += fill_cpio_trailer(buf + off);
	/* NUL padding between archives */
	memset(buf + off, 0, 4);
	off += 4;
	/* second archive */
	off += fill_cpio_entry(buf + off, CPIO_MAGIC_NEWC, "file2", 6, 0, NULL);
	off += fill_cpio_trailer(buf + off);

	result = skip_cpio_prefix(buf, off + 128);
	KUNIT_EXPECT_GE(test, result, (unsigned long)off);

	kfree(buf);
}

/*
 * Scan initrd left-to-right for cpio and erofs segments.  Unrecognized
 * non-NUL data (e.g. compressed cpio) extends to the next identifiable
 * segment boundary.  This is a test-only helper; production code uses
 * erofs_initrd_mount_layers() which integrates scanning with mounting.
 */
static int __init initrd_scan_segments(void *buf, unsigned long len,
				       struct initrd_segment *segs,
				       int max_segs, bool *has_erofs)
{
	unsigned long offset = 0;
	int count = 0;
	bool found_erofs = false;

	while (offset < len && count < max_segs) {
		unsigned long cpio_len, erofs_size, seg_end;

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

		if (!((char *)buf)[offset]) {
			offset++;
			continue;
		}

		seg_end = find_next_segment_start(buf, offset + 1, len);
		segs[count].type = INITRD_SEG_CPIO;
		segs[count].offset = offset;
		segs[count].size = seg_end - offset;
		count++;
		offset = seg_end;
	}

	if (has_erofs)
		*has_erofs = found_erofs;
	return count;
}

/* --- initrd_scan_segments tests --- */

static void __init test_scan_single_erofs(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	unsigned long img_size;
	bool has_erofs = false;
	int count;

	buf = kzalloc(16384, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 2, 0, false);

	count = initrd_scan_segments(buf, img_size, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, img_size);

	kfree(buf);
}

static void __init test_scan_multiple_erofs(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	unsigned long size1, size2;
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	size1 = place_erofs_sb(buf, 12, 4, 0, false);
	size2 = place_erofs_sb(buf + size1, 12, 2, 0, false);

	count = initrd_scan_segments(buf, size1 + size2, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, size1);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, size1);
	KUNIT_EXPECT_EQ(test, segs[1].size, size2);

	kfree(buf);
}

static void __init test_scan_48bit_erofs(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	unsigned long img_size;
	bool has_erofs = false;
	int count;

	buf = kzalloc(16384, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 2, 0, true);

	count = initrd_scan_segments(buf, img_size, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, segs[0].size, img_size);

	kfree(buf);
}

static void __init test_scan_invalid_blkszbits_low(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	bool has_erofs = false;
	int count;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	place_erofs_sb(buf, 8, 2, 0, false);

	count = initrd_scan_segments(buf, 8192, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);

	kfree(buf);
}

static void __init test_scan_invalid_blkszbits_high(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	bool has_erofs = false;
	int count;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	place_erofs_sb(buf, 31, 2, 0, false);

	count = initrd_scan_segments(buf, 8192, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);

	kfree(buf);
}

static void __init test_scan_zero_blocks(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	bool has_erofs = false;
	int count;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	place_erofs_sb(buf, 12, 0, 0, false);

	count = initrd_scan_segments(buf, 8192, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);

	kfree(buf);
}

static void __init test_scan_image_exceeds_buffer(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	unsigned long img_size;
	bool has_erofs = false;
	int count;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 4, 0, false);

	count = initrd_scan_segments(buf, img_size - 1, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);

	kfree(buf);
}

static void __init test_scan_erofs_with_trailing_garbage(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	unsigned long img_size;
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 2, 0, false);
	memset(buf + img_size, 'J', 4096);

	count = initrd_scan_segments(buf, img_size + 4096, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, img_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, img_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, 4096UL);

	kfree(buf);
}

static void __init test_scan_max_segments_limit(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	unsigned long per_img, total;
	bool has_erofs = false;
	int i, count;

	buf = kzalloc(262144, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	per_img = 0;
	for (i = 0; i < 6; i++) {
		unsigned long sz =
			place_erofs_sb(buf + per_img * i, 12, 2, 0, false);
		if (i == 0)
			per_img = sz;
	}
	total = per_img * 6;

	count = initrd_scan_segments(buf, total, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 4);
	KUNIT_EXPECT_TRUE(test, has_erofs);

	kfree(buf);
}

static void __init test_scan_no_erofs_garbage(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[2];
	bool has_erofs = false;
	int count;

	buf = kzalloc(8192, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0xFF, 8192);

	count = initrd_scan_segments(buf, 8192, segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);

	kfree(buf);
}

static void __init test_scan_buffer_too_small_for_sb(struct kunit *test)
{
	char buf[128];
	struct initrd_segment segs[2];
	bool has_erofs = false;
	int count;

	memset(buf, 0, sizeof(buf));

	count = initrd_scan_segments(buf, sizeof(buf), segs, 2, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 0);
	KUNIT_EXPECT_FALSE(test, has_erofs);
}

/* --- Mixed layout tests --- */

static void __init test_layout_cpio_then_two_erofs(struct kunit *test)
{
	char *buf;
	size_t cpio_off;
	unsigned long size1, size2;
	struct initrd_segment segs[8];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	cpio_off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC,
				   "kernel/x86/microcode/GenuineIntel.bin", 38,
				   8, "\x01\x02\x03\x04\x05\x06\x07\x08");
	cpio_off += fill_cpio_trailer(buf + cpio_off);

	size1 = place_erofs_sb(buf + cpio_off, 12, 4, 0, false);
	size2 = place_erofs_sb(buf + cpio_off + size1, 12, 2, 0, false);

	count = initrd_scan_segments(buf, cpio_off + size1 + size2, segs, 8,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)cpio_off);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, (unsigned long)cpio_off);
	KUNIT_EXPECT_EQ(test, segs[1].size, size1);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset,
			(unsigned long)(cpio_off + size1));
	KUNIT_EXPECT_EQ(test, segs[2].size, size2);

	kfree(buf);
}

static void __init test_layout_erofs_then_cpio(struct kunit *test)
{
	char *buf;
	unsigned long img_size;
	size_t cpio_start, cpio_end;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 2, 0, false);

	cpio_start = img_size;
	cpio_end = cpio_start;
	cpio_end += fill_cpio_entry(buf + cpio_end, CPIO_MAGIC_NEWC, "extra", 6,
				    3, "abc");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	count = initrd_scan_segments(buf, cpio_end, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, img_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, (unsigned long)cpio_start);
	KUNIT_EXPECT_EQ(test, segs[1].size,
			(unsigned long)(cpio_end - cpio_start));

	kfree(buf);
}

static void __init test_layout_cpio_erofs_cpio(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long cpio1_size, erofs_size, cpio2_start, cpio2_size;
	struct initrd_segment segs[8];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "file1", 6, 4, "data");
	off += fill_cpio_trailer(buf + off);
	cpio1_size = off;

	erofs_size = place_erofs_sb(buf + off, 12, 2, 0, false);
	off += erofs_size;

	cpio2_start = off;
	off += fill_cpio_entry(buf + off, CPIO_MAGIC_NEWC, "file2", 6, 5,
			       "hello");
	off += fill_cpio_trailer(buf + off);
	cpio2_size = off - cpio2_start;

	count = initrd_scan_segments(buf, off, segs, 8, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, cpio1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, cpio1_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[2].offset, cpio2_start);
	KUNIT_EXPECT_EQ(test, segs[2].size, cpio2_size);

	kfree(buf);
}

static void __init test_layout_erofs_cpio_erofs(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long erofs1_size, cpio_start, cpio_size, erofs2_off,
		erofs2_size;
	struct initrd_segment segs[8];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = 0;
	erofs1_size = place_erofs_sb(buf, 12, 2, 0, false);
	off += erofs1_size;

	cpio_start = off;
	off += fill_cpio_entry(buf + off, CPIO_MAGIC_NEWC, "overlay", 8, 0,
			       NULL);
	off += fill_cpio_trailer(buf + off);
	cpio_size = off - cpio_start;

	erofs2_off = off;
	erofs2_size = place_erofs_sb(buf + off, 12, 3, 0, false);
	off += erofs2_size;

	count = initrd_scan_segments(buf, off, segs, 8, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, erofs1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, cpio_start);
	KUNIT_EXPECT_EQ(test, segs[1].size, cpio_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset, erofs2_off);
	KUNIT_EXPECT_EQ(test, segs[2].size, erofs2_size);

	kfree(buf);
}

static void __init test_layout_cpio_erofs_erofs_cpio(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long c1_size, e1_size, e2_off, e2_size, c2_off, c2_size;
	struct initrd_segment segs[8];
	bool has_erofs = false;
	int count;

	buf = kzalloc(131072, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "early", 6, 0, NULL);
	off += fill_cpio_trailer(buf + off);
	c1_size = off;

	e1_size = place_erofs_sb(buf + off, 12, 4, 0, false);
	off += e1_size;

	e2_off = off;
	e2_size = place_erofs_sb(buf + off, 12, 2, 0, false);
	off += e2_size;

	c2_off = off;
	off += fill_cpio_entry(buf + off, CPIO_MAGIC_NEWC, "late", 5, 0, NULL);
	off += fill_cpio_trailer(buf + off);
	c2_size = off - c2_off;

	count = initrd_scan_segments(buf, off, segs, 8, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 4);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, c1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].size, e1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset, e2_off);
	KUNIT_EXPECT_EQ(test, segs[2].size, e2_size);
	KUNIT_EXPECT_EQ(test, (int)segs[3].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[3].offset, c2_off);
	KUNIT_EXPECT_EQ(test, segs[3].size, c2_size);

	kfree(buf);
}

static void __init test_layout_only_cpio(struct kunit *test)
{
	char *buf;
	size_t off;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "justcpio", 9, 0, NULL);
	off += fill_cpio_trailer(buf + off);

	count = initrd_scan_segments(buf, off, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)off);

	kfree(buf);
}

static void __init test_layout_cpio_nul_padded_then_erofs(struct kunit *test)
{
	char *buf;
	size_t cpio_off, erofs_off;
	unsigned long erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(16384, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	cpio_off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC,
				   "kernel/x86/microcode/GenuineIntel.bin", 38,
				   4, "\xde\xad");
	cpio_off += fill_cpio_trailer(buf + cpio_off);

	erofs_off = ALIGN(cpio_off, 512);
	memset(buf + cpio_off, 0, erofs_off - cpio_off);

	erofs_size = place_erofs_sb(buf + erofs_off, 12, 2, 0, false);

	count = initrd_scan_segments(buf, erofs_off + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)cpio_off);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, (unsigned long)erofs_off);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

static void __init test_layout_erofs_then_compressed_blob(struct kunit *test)
{
	char *buf;
	unsigned long img_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	img_size = place_erofs_sb(buf, 12, 2, 0, false);

	/*
	 * Trailing non-cpio, non-erofs data (e.g. compressed cpio):
	 * the scanner treats it as SEG_CPIO since its size can't be
	 * determined without decompression.
	 */
	memset(buf + img_size, 0xAB, 2048);

	count = initrd_scan_segments(buf, img_size + 2048, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, img_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, img_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, 2048UL);

	kfree(buf);
}

static void __init test_skip_cpio_truncated_entry(struct kunit *test)
{
	char *buf;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/*
	 * Valid header claiming a 64 KiB file, but only 256 bytes of
	 * buffer remain after the header — exercises the next64 > len
	 * bounds check.
	 */
	fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "huge", 5, 65536, NULL);

	result = skip_cpio_prefix(buf, 256);
	KUNIT_EXPECT_EQ(test, result, 0UL);

	kfree(buf);
}

static void __init test_scan_all_nul_buffer(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	count = initrd_scan_segments(buf, 4096, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 0);
	KUNIT_EXPECT_FALSE(test, has_erofs);

	kfree(buf);
}

/*
 * Regression test for the NUL-scan fix: place >1024 NUL bytes before
 * an EROFS image.  The old blanket-skip logic consumed all zeros
 * including the 1024-byte EROFS reserved area, causing the scanner to
 * miss the superblock.  The byte-at-a-time advance must find it.
 */
static void __init test_scan_large_nul_gap_then_erofs(struct kunit *test)
{
	char *buf;
	struct initrd_segment segs[4];
	unsigned long erofs_size;
	unsigned long gap = 2048;
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0, gap);
	erofs_size = place_erofs_sb(buf + gap, 12, 2, 0, false);

	count = initrd_scan_segments(buf, gap + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].offset, gap);
	KUNIT_EXPECT_EQ(test, segs[0].size, erofs_size);

	kfree(buf);
}

static void __init test_layout_compressed_blob_then_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 4096, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0xAB, blob_size);
	erofs_size = place_erofs_sb(buf + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, blob_size + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

static void __init test_layout_erofs_compressed_blob_erofs(struct kunit *test)
{
	char *buf;
	unsigned long e1_size, blob_off, blob_size = 2048, e2_off, e2_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	e1_size = place_erofs_sb(buf, 12, 2, 0, false);
	blob_off = e1_size;
	memset(buf + blob_off, 0xCD, blob_size);
	e2_off = blob_off + blob_size;
	e2_size = place_erofs_sb(buf + e2_off, 12, 3, 0, false);

	count = initrd_scan_segments(buf, e2_off + e2_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, e1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_off);
	KUNIT_EXPECT_EQ(test, segs[1].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset, e2_off);
	KUNIT_EXPECT_EQ(test, segs[2].size, e2_size);

	kfree(buf);
}

static void __init test_layout_compressed_blob_nul_gap_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 3072, gap = 512, erofs_off, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0xEF, blob_size);
	memset(buf + blob_size, 0, gap);
	erofs_off = blob_size + gap;
	erofs_size = place_erofs_sb(buf + erofs_off, 12, 2, 0, false);

	count = initrd_scan_segments(buf, erofs_off + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, erofs_off);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

static void __init
test_layout_compressed_blob_then_uncompressed_cpio(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 2048;
	size_t cpio_off, cpio_end;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0xAB, blob_size);
	cpio_off = blob_size;
	cpio_end = cpio_off;
	cpio_end += fill_cpio_entry(buf + cpio_end, CPIO_MAGIC_NEWC, "file", 5,
				    3, "abc");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	count = initrd_scan_segments(buf, cpio_end, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);
	KUNIT_EXPECT_EQ(test, segs[1].size,
			(unsigned long)(cpio_end - cpio_off));

	kfree(buf);
}

/* --- Compressed format magic tests --- */

/*
 * Real gzip-compressed cpio (magic \x1f\x8b) followed by EROFS.
 * Scanner must not get confused by compression magic and still find
 * the trailing EROFS image.
 */
static void __init test_layout_gzip_cpio_then_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 3500, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	buf[0] = 0x1f;
	buf[1] = 0x8b;
	buf[2] = 0x08; /* deflate method */
	memset(buf + 3, 0x42, blob_size - 3);
	erofs_size = place_erofs_sb(buf + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, blob_size + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

static void __init test_layout_xz_cpio_then_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 5000, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* xz magic: fd 37 7a 58 5a 00 */
	buf[0] = 0xfd;
	buf[1] = 0x37;
	buf[2] = 0x7a;
	buf[3] = 0x58;
	buf[4] = 0x5a;
	buf[5] = 0x00;
	memset(buf + 6, 0x55, blob_size - 6);
	erofs_size = place_erofs_sb(buf + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, blob_size + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);

	kfree(buf);
}

static void __init test_layout_zstd_cpio_then_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 6144, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* zstd magic: 28 b5 2f fd */
	buf[0] = 0x28;
	buf[1] = 0xb5;
	buf[2] = 0x2f;
	buf[3] = 0xfd;
	memset(buf + 4, 0x77, blob_size - 4);
	erofs_size = place_erofs_sb(buf + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, blob_size + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);

	kfree(buf);
}

static void __init test_layout_lz4_cpio_then_erofs(struct kunit *test)
{
	char *buf;
	unsigned long blob_size = 2500, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* lz4 legacy magic: 02 21 4c 18 */
	buf[0] = 0x02;
	buf[1] = 0x21;
	buf[2] = 0x4c;
	buf[3] = 0x18;
	memset(buf + 4, 0x33, blob_size - 4);
	erofs_size = place_erofs_sb(buf + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, blob_size + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);

	kfree(buf);
}

/*
 * Interleaved: uncompressed cpio, gzip cpio, EROFS, zstd cpio, EROFS.
 * Exercises the full spectrum of segment types in a single buffer.
 */
static void __init test_layout_mixed_compress_formats(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long c1_size, gz_size = 3000, e1_off, e1_size;
	unsigned long zst_off, zst_size = 2000, e2_off, e2_size;
	struct initrd_segment segs[8];
	bool has_erofs = false;
	int count;

	buf = kzalloc(131072, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "micro", 6, 4, "uuuu");
	off += fill_cpio_trailer(buf + off);
	c1_size = off;

	/* gzip blob */
	buf[off] = 0x1f;
	buf[off + 1] = 0x8b;
	memset(buf + off + 2, 0x44, gz_size - 2);
	off += gz_size;

	e1_off = off;
	e1_size = place_erofs_sb(buf + off, 12, 2, 0, false);
	off += e1_size;

	/* zstd blob */
	zst_off = off;
	buf[off] = 0x28;
	buf[off + 1] = 0xb5;
	buf[off + 2] = 0x2f;
	buf[off + 3] = 0xfd;
	memset(buf + off + 4, 0x66, zst_size - 4);
	off += zst_size;

	e2_off = off;
	e2_size = place_erofs_sb(buf + off, 12, 3, 0, false);
	off += e2_size;

	count = initrd_scan_segments(buf, off, segs, 8, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 5);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, c1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, c1_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, gz_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset, e1_off);
	KUNIT_EXPECT_EQ(test, segs[2].size, e1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[3].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[3].offset, zst_off);
	KUNIT_EXPECT_EQ(test, segs[3].size, zst_size);
	KUNIT_EXPECT_EQ(test, (int)segs[4].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[4].offset, e2_off);
	KUNIT_EXPECT_EQ(test, segs[4].size, e2_size);

	kfree(buf);
}

/* --- Page-misaligned boundary tests --- */

/*
 * EROFS at an offset that is not a multiple of any common page size
 * (4K, 16K, 64K).  The scanner must find it regardless.
 */
static void __init test_scan_erofs_at_non_page_offset(struct kunit *test)
{
	char *buf;
	unsigned long gap = 5000, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0, gap);
	erofs_size = place_erofs_sb(buf + gap, 12, 2, 0, false);

	count = initrd_scan_segments(buf, gap + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].offset, gap);
	KUNIT_EXPECT_EQ(test, segs[0].size, erofs_size);

	kfree(buf);
}

/*
 * Two EROFS images back-to-back where the first has a non-page-aligned
 * size (3 blocks of 4096 = 12288), meaning the second starts mid-page
 * on any system with PAGE_SIZE > 4096.
 */
static void __init test_layout_erofs_pair_nonpage_boundary(struct kunit *test)
{
	char *buf;
	unsigned long e1_size, e2_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(131072, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	e1_size = place_erofs_sb(buf, 12, 3, 0, false);
	/* e1_size = 3 * 4096 = 12288, not 16K/64K aligned */
	e2_size = place_erofs_sb(buf + e1_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, e1_size + e2_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, e1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, e1_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, e2_size);

	kfree(buf);
}

/*
 * Cpio of odd (non-page-aligned) size immediately followed by EROFS.
 * The cpio fill_cpio_entry aligns to 4 bytes, so the boundary falls
 * at an address that is 4-byte-aligned but NOT page-aligned.
 */
static void __init test_layout_cpio_odd_size_then_erofs(struct kunit *test)
{
	char *buf;
	size_t cpio_end;
	unsigned long erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* 37-byte filename + 13 bytes of data → odd offset after alignment */
	cpio_end = fill_cpio_entry(buf, CPIO_MAGIC_NEWC,
				   "a/really/long/path/to/some/file.txt", 36,
				   13, "hello world!\n");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	KUNIT_ASSERT_NE(test, cpio_end % PAGE_SIZE, 0UL);

	erofs_size = place_erofs_sb(buf + cpio_end, 12, 2, 0, false);

	count = initrd_scan_segments(buf, cpio_end + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)cpio_end);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, (unsigned long)cpio_end);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

/*
 * Compressed blob at non-page-aligned offset followed by EROFS at
 * another non-page-aligned offset.  Both boundaries are misaligned.
 */
static void __init
test_layout_misaligned_compressed_then_erofs(struct kunit *test)
{
	char *buf;
	size_t cpio_end;
	unsigned long blob_size = 5001, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	cpio_end = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "micro", 6, 8,
				   "\x01\x02\x03\x04\x05\x06\x07\x08");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	/* gzip blob of non-page-aligned size */
	buf[cpio_end] = 0x1f;
	buf[cpio_end + 1] = 0x8b;
	memset(buf + cpio_end + 2, 0x99, blob_size - 2);

	erofs_size =
		place_erofs_sb(buf + cpio_end + blob_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, cpio_end + blob_size + erofs_size,
				     segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)cpio_end);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, (unsigned long)cpio_end);
	KUNIT_EXPECT_EQ(test, segs[1].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[2].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[2].offset,
			(unsigned long)(cpio_end + blob_size));
	KUNIT_EXPECT_EQ(test, segs[2].size, erofs_size);

	kfree(buf);
}

/*
 * EROFS with 512-byte block size (blkszbits=9, the minimum allowed).
 * Image size is 512*5 = 2560 bytes — not a multiple of any page size.
 * Second EROFS follows immediately at that misaligned offset.
 */
static void __init test_layout_small_block_erofs_pair(struct kunit *test)
{
	char *buf;
	unsigned long e1_size, e2_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	e1_size = place_erofs_sb(buf, 9, 5, 0, false);
	/* e1_size = 5 * 512 = 2560, very misaligned */
	KUNIT_ASSERT_EQ(test, e1_size, 2560UL);

	e2_size = place_erofs_sb(buf + e1_size, 12, 2, 0, false);

	count = initrd_scan_segments(buf, e1_size + e2_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[0].size, e1_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, e1_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, e2_size);

	kfree(buf);
}

/*
 * EROFS at offset PAGE_SIZE - 1: the worst-case sub-page misalignment.
 * Preceded by non-NUL garbage so the scanner enters the "unrecognized
 * data" path and must scan byte-by-byte to find the EROFS magic.
 */
static void __init test_scan_erofs_at_page_minus_one(struct kunit *test)
{
	char *buf;
	unsigned long gap = PAGE_SIZE - 1, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(PAGE_SIZE + 65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, 0xBB, gap);
	erofs_size = place_erofs_sb(buf + gap, 12, 2, 0, false);

	count = initrd_scan_segments(buf, gap + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, gap);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, gap);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

/*
 * Seven-byte NUL gap between an uncompressed cpio and EROFS — tests
 * the smallest realistic misalignment from concatenation tools that
 * pad to e.g. 512-byte boundaries.
 */
static void __init test_layout_tiny_nul_gap_cpio_erofs(struct kunit *test)
{
	char *buf;
	size_t cpio_end;
	unsigned long erofs_off, erofs_size;
	struct initrd_segment segs[4];
	bool has_erofs = false;
	int count;

	buf = kzalloc(65536, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	cpio_end = fill_cpio_entry(buf, CPIO_MAGIC_NEWC, "f", 2, 1, "x");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	erofs_off = cpio_end + 7;
	memset(buf + cpio_end, 0, 7);
	erofs_size = place_erofs_sb(buf + erofs_off, 12, 2, 0, false);

	count = initrd_scan_segments(buf, erofs_off + erofs_size, segs, 4,
				     &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_TRUE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].size, (unsigned long)cpio_end);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_EROFS);
	KUNIT_EXPECT_EQ(test, segs[1].offset, erofs_off);
	KUNIT_EXPECT_EQ(test, segs[1].size, erofs_size);

	kfree(buf);
}

static struct kunit_case __refdata do_mounts_erofs_test_cases[] = {
	/* skip_cpio_prefix */
	KUNIT_CASE(test_skip_cpio_no_cpio),
	KUNIT_CASE(test_skip_cpio_garbage),
	KUNIT_CASE(test_skip_cpio_too_short),
	KUNIT_CASE(test_skip_cpio_single_with_trailer),
	KUNIT_CASE(test_skip_cpio_with_filedata),
	KUNIT_CASE(test_skip_cpio_070702_magic),
	KUNIT_CASE(test_skip_cpio_nul_padding_no_next),
	KUNIT_CASE(test_skip_cpio_no_trailer),
	KUNIT_CASE(test_skip_cpio_concatenated),
	KUNIT_CASE(test_skip_cpio_truncated_entry),
	/* initrd_scan_segments */
	KUNIT_CASE(test_scan_single_erofs),
	KUNIT_CASE(test_scan_multiple_erofs),
	KUNIT_CASE(test_scan_48bit_erofs),
	KUNIT_CASE(test_scan_invalid_blkszbits_low),
	KUNIT_CASE(test_scan_invalid_blkszbits_high),
	KUNIT_CASE(test_scan_zero_blocks),
	KUNIT_CASE(test_scan_image_exceeds_buffer),
	KUNIT_CASE(test_scan_erofs_with_trailing_garbage),
	KUNIT_CASE(test_scan_max_segments_limit),
	KUNIT_CASE(test_scan_no_erofs_garbage),
	KUNIT_CASE(test_scan_buffer_too_small_for_sb),
	KUNIT_CASE(test_scan_all_nul_buffer),
	KUNIT_CASE(test_scan_large_nul_gap_then_erofs),
	/* Mixed layout */
	KUNIT_CASE(test_layout_cpio_then_two_erofs),
	KUNIT_CASE(test_layout_erofs_then_cpio),
	KUNIT_CASE(test_layout_cpio_erofs_cpio),
	KUNIT_CASE(test_layout_erofs_cpio_erofs),
	KUNIT_CASE(test_layout_cpio_erofs_erofs_cpio),
	KUNIT_CASE(test_layout_only_cpio),
	KUNIT_CASE(test_layout_cpio_nul_padded_then_erofs),
	KUNIT_CASE(test_layout_erofs_then_compressed_blob),
	KUNIT_CASE(test_layout_compressed_blob_then_erofs),
	KUNIT_CASE(test_layout_erofs_compressed_blob_erofs),
	KUNIT_CASE(test_layout_compressed_blob_nul_gap_erofs),
	KUNIT_CASE(test_layout_compressed_blob_then_uncompressed_cpio),
	/* Compressed format magic */
	KUNIT_CASE(test_layout_gzip_cpio_then_erofs),
	KUNIT_CASE(test_layout_xz_cpio_then_erofs),
	KUNIT_CASE(test_layout_zstd_cpio_then_erofs),
	KUNIT_CASE(test_layout_lz4_cpio_then_erofs),
	KUNIT_CASE(test_layout_mixed_compress_formats),
	/* Page-misaligned boundaries */
	KUNIT_CASE(test_scan_erofs_at_non_page_offset),
	KUNIT_CASE(test_layout_erofs_pair_nonpage_boundary),
	KUNIT_CASE(test_layout_cpio_odd_size_then_erofs),
	KUNIT_CASE(test_layout_misaligned_compressed_then_erofs),
	KUNIT_CASE(test_layout_small_block_erofs_pair),
	KUNIT_CASE(test_scan_erofs_at_page_minus_one),
	KUNIT_CASE(test_layout_tiny_nul_gap_cpio_erofs),
	{},
};

static struct kunit_suite do_mounts_erofs_test_suite = {
	.name = "initrd_segments",
	.test_cases = do_mounts_erofs_test_cases,
};
kunit_test_init_section_suites(&do_mounts_erofs_test_suite);

MODULE_DESCRIPTION("Initrd segment scanning KUnit tests");
MODULE_LICENSE("GPL");
