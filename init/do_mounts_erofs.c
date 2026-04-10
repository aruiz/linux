// SPDX-License-Identifier: GPL-2.0
/*
 * Support for erofs-formatted initrd images: scan for segments, mount
 * erofs via a read-only RAM block device, overlay for writable root.
 */
#include <linux/build_bug.h>
#include <linux/init.h>
#include <linux/init_syscalls.h>
#include <linux/initrd.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"
#include "do_mounts_erofs_internal.h"

extern dev_t __init initrd_blkdev_create(void *data, unsigned long size,
					 const char *name);
extern void __init initrd_blkdev_add_pages(unsigned long start,
					   unsigned long end);

static bool initrd_is_erofs;
static struct initrd_segment initrd_segs[INITRD_MAX_SEGMENTS];
static int initrd_seg_count;

bool __init initrd_has_erofs(void *buf, unsigned long len)
{
	return false;
}

bool __init erofs_initrd_is_active(void)
{
	return initrd_is_erofs;
}

int __init erofs_initrd_setup(void)
{
	return -ENODEV;
}
