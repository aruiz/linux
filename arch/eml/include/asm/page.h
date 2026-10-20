/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_PAGE_H
#define _ASM_EML_PAGE_H

/* Use CONFIG_PAGE_SHIFT from vdso/page.h to avoid redefinition */
#include <vdso/page.h>

#define PAGE_OFFSET 0UL

#ifndef __ASSEMBLY__

/* NOMMU: identity mapping, virtual == physical */
#define __pa(x) ((unsigned long)(x))
#define __va(x) ((void *)((unsigned long)(x)))

#define virt_to_pfn(addr) (__pa(addr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn) __va((pfn) << PAGE_SHIFT)

#define virt_to_page(addr) pfn_to_page(virt_to_pfn(addr))
#define page_to_virt(page) pfn_to_virt(page_to_pfn(page))

#define pfn_valid(pfn) ((pfn) < max_mapnr)
#define virt_addr_valid(kaddr) pfn_valid(virt_to_pfn(kaddr))

#define clear_page(page) memset((page), 0, PAGE_SIZE)
#define copy_page(to, from) memcpy((to), (from), PAGE_SIZE)
#define copy_user_page(to, from, vaddr, pg) copy_page(to, from)

/* NOMMU: pgtable_t is not used, but mm_types.h needs a typedef */
typedef struct page *pgtable_t;

typedef struct {
	unsigned long pte;
} pte_t;

typedef struct {
	unsigned long pgd;
} pgd_t;

typedef struct {
	unsigned long pgprot;
} pgprot_t;

#define pte_val(x) ((x).pte)
#define pgd_val(x) ((x).pgd)
#define pgprot_val(x) ((x).pgprot)

#define __pte(x) ((pte_t){ (x) })
#define __pgd(x) ((pgd_t){ (x) })
#define __pgprot(x) ((pgprot_t){ (x) })

extern unsigned long max_mapnr;

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_EML_PAGE_H */
