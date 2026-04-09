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
	return fill_cpio_entry(out, "070701", "TRAILER!!!", 11, 0, NULL);
}

/*
 * Place a minimal EROFS superblock at @buf.  The buffer must be at
 * least EROFS_SUPER_OFFSET + sizeof(struct erofs_initrd_sb) bytes.
 * Returns the total image size implied by @blocks / @blkszbits.
 */
static unsigned long __init place_erofs_sb(void *buf, u8 blkszbits,
					   u32 blocks_lo, u16 blocks_hi,
					   bool use_48bit)
{
	struct erofs_initrd_sb *sb = buf + EROFS_SUPER_OFFSET;

	memset(buf, 0, EROFS_SUPER_OFFSET + sizeof(*sb));
	sb->magic = cpu_to_le32(EROFS_SUPER_MAGIC_V1);
	sb->blkszbits = blkszbits;
	sb->blocks_lo = cpu_to_le32(blocks_lo);
	if (use_48bit) {
		sb->feature_incompat =
			cpu_to_le32(EROFS_FEATURE_INCOMPAT_48BIT);
		sb->rb_field = cpu_to_le16(blocks_hi);
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

	memcpy(buf, "070701", 6);
	KUNIT_EXPECT_EQ(test, skip_cpio_prefix(buf, sizeof(buf)), 0UL);
}

static void __init test_skip_cpio_single_with_trailer(struct kunit *test)
{
	char *buf;
	size_t off;
	unsigned long result;

	buf = kzalloc(4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	off = fill_cpio_entry(buf, "070701", "testfile", 9, 4, "data");
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

	off = fill_cpio_entry(buf, "070701", "bigfile", 8, strlen(payload),
			      payload);
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

	off = fill_cpio_entry(buf, "070702", "csumfile", 9, 0, NULL);
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

	off = fill_cpio_entry(buf, "070701", "afile", 6, 0, NULL);
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

	off = fill_cpio_entry(buf, "070701", "notrailer", 10, 0, NULL);
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

	off = fill_cpio_entry(buf, "070701", "file1", 6, 3, "abc");
	off += fill_cpio_trailer(buf + off);
	/* NUL padding between archives */
	memset(buf + off, 0, 4);
	off += 4;
	/* second archive */
	off += fill_cpio_entry(buf + off, "070701", "file2", 6, 0, NULL);
	off += fill_cpio_trailer(buf + off);

	result = skip_cpio_prefix(buf, off + 128);
	KUNIT_EXPECT_GE(test, result, (unsigned long)off);

	kfree(buf);
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

	cpio_off = fill_cpio_entry(buf, "070701",
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
	cpio_end +=
		fill_cpio_entry(buf + cpio_end, "070701", "extra", 6, 3, "abc");
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

	off = fill_cpio_entry(buf, "070701", "file1", 6, 4, "data");
	off += fill_cpio_trailer(buf + off);
	cpio1_size = off;

	erofs_size = place_erofs_sb(buf + off, 12, 2, 0, false);
	off += erofs_size;

	cpio2_start = off;
	off += fill_cpio_entry(buf + off, "070701", "file2", 6, 5, "hello");
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
	off += fill_cpio_entry(buf + off, "070701", "overlay", 8, 0, NULL);
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

	off = fill_cpio_entry(buf, "070701", "early", 6, 0, NULL);
	off += fill_cpio_trailer(buf + off);
	c1_size = off;

	e1_size = place_erofs_sb(buf + off, 12, 4, 0, false);
	off += e1_size;

	e2_off = off;
	e2_size = place_erofs_sb(buf + off, 12, 2, 0, false);
	off += e2_size;

	c2_off = off;
	off += fill_cpio_entry(buf + off, "070701", "late", 5, 0, NULL);
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

	off = fill_cpio_entry(buf, "070701", "justcpio", 9, 0, NULL);
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

	cpio_off = fill_cpio_entry(buf, "070701",
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
	fill_cpio_entry(buf, "070701", "huge", 5, 65536, NULL);

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
	cpio_end +=
		fill_cpio_entry(buf + cpio_end, "070701", "file", 5, 3, "abc");
	cpio_end += fill_cpio_trailer(buf + cpio_end);

	count = initrd_scan_segments(buf, cpio_end, segs, 4, &has_erofs);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_FALSE(test, has_erofs);
	KUNIT_EXPECT_EQ(test, (int)segs[0].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[0].offset, 0UL);
	KUNIT_EXPECT_EQ(test, segs[0].size, blob_size);
	KUNIT_EXPECT_EQ(test, (int)segs[1].type, (int)INITRD_SEG_CPIO);
	KUNIT_EXPECT_EQ(test, segs[1].offset, blob_size);
	KUNIT_EXPECT_EQ(test, segs[1].size, (unsigned long)(cpio_end - cpio_off));

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
	{},
};

static struct kunit_suite do_mounts_erofs_test_suite = {
	.name = "initrd_segments",
	.test_cases = do_mounts_erofs_test_cases,
};
kunit_test_init_section_suites(&do_mounts_erofs_test_suite);

MODULE_DESCRIPTION("Initrd segment scanning KUnit tests");
MODULE_LICENSE("GPL");
