// SPDX-License-Identifier: GPL-2.0
#ifndef __INITRAMFS_INTERNAL_H__
#define __INITRAMFS_INTERNAL_H__

char *unpack_to_rootfs(char *buf, unsigned long len, unsigned long *consumed);
#define CPIO_HDRLEN 110

#ifdef CONFIG_INITRD_EROFS
#define EROFS_BLKSZBITS_MIN 9 /* 512 bytes */
#define EROFS_BLKSZBITS_MAX 30 /* 1 GiB */

unsigned long try_parse_erofs(void *buf, unsigned long off, unsigned long len);
#endif

#endif
