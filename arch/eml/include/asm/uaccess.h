/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_UACCESS_H
#define _ASM_EML_UACCESS_H

/*
 * EML has no userspace so user/kernel addresses are the same.
 * This is equivalent to NOMMU uaccess.
 */

#include <linux/string.h>

#define MAKE_MM_SEG(s) ((mm_segment_t){ (s) })
#define KERNEL_DS MAKE_MM_SEG(~0UL)
#define USER_DS MAKE_MM_SEG(TASK_SIZE)

#define get_fs() (KERNEL_DS)
#define uaccess_kernel() (1)

#define access_ok(addr, size) (1)

static inline unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline unsigned long raw_copy_to_user(void __user *to, const void *from,
					     unsigned long n)
{
	memcpy((void __force *)to, from, n);
	return 0;
}

#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>

#endif /* _ASM_EML_UACCESS_H */
