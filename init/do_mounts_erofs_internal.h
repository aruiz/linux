/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DO_MOUNTS_EROFS_INTERNAL_H__
#define __DO_MOUNTS_EROFS_INTERNAL_H__

#include <linux/types.h>

#define EROFS_SUPER_OFFSET 1024
#define EROFS_FEATURE_INCOMPAT_48BIT 0x00000080
#define INITRD_MAX_SEGMENTS 32
#define CPIO_HDRLEN 110

struct erofs_initrd_sb {
	__le32 magic;
	__le32 checksum;
	__le32 feature_compat;
	__u8 blkszbits;
	__u8 sb_extslots;
	__le16 rb_field; /* rootnid_2b or blocks_hi */
	__le64 inos;
	__le64 epoch;
	__le32 fixed_nsec;
	__le32 blocks_lo;
	__le32 meta_blkaddr;
	__le32 xattr_blkaddr;
	__u8 uuid[16];
	__u8 volume_name[16];
	__le32 feature_incompat;
};

enum initrd_seg_type { INITRD_SEG_CPIO, INITRD_SEG_EROFS };

struct initrd_segment {
	enum initrd_seg_type type;
	unsigned long offset; /* relative to initrd_start */
	unsigned long size;
};

unsigned long __init skip_cpio_prefix(void *data, unsigned long len);

int __init initrd_scan_segments(void *buf, unsigned long len,
				struct initrd_segment *segs, int max_segs,
				bool *has_erofs);

#endif
