/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DO_MOUNTS_EROFS_INTERNAL_H__
#define __DO_MOUNTS_EROFS_INTERNAL_H__

#include <linux/string.h>
#include <linux/types.h>
#include "../fs/erofs/erofs_fs.h"

#define INITRD_MAX_SEGMENTS 32
#define CPIO_HDRLEN 110
#define CPIO_MAGIC_NEWC "070701"
#define CPIO_MAGIC_CRC "070702"
#define CPIO_MAGIC_LEN 6
#define CPIO_FIELD_LEN 8
#define CPIO_OFF_FILESIZE 54
#define CPIO_OFF_NAMESIZE 94
#define CPIO_TRAILER "TRAILER!!!"

static inline bool is_cpio_newc_hdr(const void *p)
{
	return memcmp(p, CPIO_MAGIC_NEWC, CPIO_MAGIC_LEN) == 0 ||
	       memcmp(p, CPIO_MAGIC_CRC, CPIO_MAGIC_LEN) == 0;
}

enum initrd_seg_type { INITRD_SEG_CPIO, INITRD_SEG_EROFS };

struct initrd_segment {
	enum initrd_seg_type type;
	unsigned long offset; /* relative to initrd_start */
	unsigned long size;
};

unsigned long __init skip_cpio_prefix(void *data, unsigned long len);

unsigned long __init try_parse_erofs(void *buf, unsigned long offset,
				     unsigned long len);

#endif
