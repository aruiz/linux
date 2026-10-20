/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_PGTABLE_H
#define _ASM_EML_PGTABLE_H

#include <asm-generic/pgtable-nopmd.h>
#include <asm/page.h>

/* NOMMU: page protection values are meaningless, define as zero */
#define PAGE_KERNEL __pgprot(0)
#define PAGE_KERNEL_RO __pgprot(0)
#define PAGE_KERNEL_EXEC __pgprot(0)

#define swapper_pg_dir ((pgd_t *)0)

#define VMALLOC_START 0UL
#define VMALLOC_END 0xffffffffffffffffUL

#endif /* _ASM_EML_PGTABLE_H */
