// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/efi.h>
#include <asm/eml_efi.h>
#include <asm/page.h>
#include <asm/sections.h>

/*
 * Allocate memory from UEFI Boot Services so the firmware won't reuse it,
 * then hand it to Linux's memblock allocator.  This avoids the conflict
 * where both Linux and UEFI allocate from the same EFI_CONVENTIONAL_MEMORY
 * pool (since EML never calls ExitBootServices).
 */

#define EML_MEM_CHUNK_PAGES 4096 /* 16 MB per allocation */
#define EML_MEM_MAX_CHUNKS 16

void __init eml_setup_memory(void)
{
	efi_status_t status;
	u64 mem_end = 0;
	int count = 0;
	int i;
	u64 chunks[EML_MEM_MAX_CHUNKS];
	unsigned long chunk_pages[EML_MEM_MAX_CHUNKS];
	int nchunks = 0;
	unsigned long pages_to_try = EML_MEM_CHUNK_PAGES;

	/*
	 * Grab as much memory as possible from UEFI in large chunks.
	 * AllocatePages with EFI_ALLOCATE_ANY_PAGES lets the firmware
	 * pick the address; the memory is marked allocated in the EFI
	 * map so firmware won't hand it out again.
	 */
	while (nchunks < EML_MEM_MAX_CHUNKS && pages_to_try >= 16) {
		u64 addr = 0;

		status = eml_systab->boot_services->allocate_pages(
			EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages_to_try,
			&addr);

		if (status != EFI_SUCCESS) {
			pages_to_try /= 2;
			continue;
		}

		chunks[nchunks] = addr;
		chunk_pages[nchunks] = pages_to_try;
		nchunks++;
	}

	/* Add the allocated chunks to memblock */
	for (i = 0; i < nchunks; i++) {
		u64 start = chunks[i];
		u64 size = chunk_pages[i] << EFI_PAGE_SHIFT;

		memblock_add(start, size);
		if (start + size > mem_end)
			mem_end = start + size;
		count++;
	}

	/* Reserve the kernel image itself */
	memblock_reserve(__pa(_text), __pa(_end) - __pa(_text));

	max_pfn = PFN_DOWN(mem_end);
	max_low_pfn = max_pfn;
	min_low_pfn = PFN_UP(__pa(_end));
	max_mapnr = max_pfn;
}
