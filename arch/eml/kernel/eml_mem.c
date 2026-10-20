// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/efi.h>
#include <asm/eml_efi.h>
#include <asm/page.h>
#include <asm/sections.h>

static const char *eml_mem_type_name(u32 type)
{
	static const char *const names[] = {
		[EFI_RESERVED_TYPE] = "Reserved",
		[EFI_LOADER_CODE] = "Loader Code",
		[EFI_LOADER_DATA] = "Loader Data",
		[EFI_BOOT_SERVICES_CODE] = "Boot Code",
		[EFI_BOOT_SERVICES_DATA] = "Boot Data",
		[EFI_RUNTIME_SERVICES_CODE] = "Runtime Code",
		[EFI_RUNTIME_SERVICES_DATA] = "Runtime Data",
		[EFI_CONVENTIONAL_MEMORY] = "Conventional",
		[EFI_UNUSABLE_MEMORY] = "Unusable",
		[EFI_ACPI_RECLAIM_MEMORY] = "ACPI Reclaim",
		[EFI_ACPI_MEMORY_NVS] = "ACPI NVS",
		[EFI_MEMORY_MAPPED_IO] = "MMIO",
		[EFI_MEMORY_MAPPED_IO_PORT_SPACE] = "MMIO Port",
		[EFI_PAL_CODE] = "PAL Code",
		[EFI_PERSISTENT_MEMORY] = "Persistent",
	};

	if (type < ARRAY_SIZE(names) && names[type])
		return names[type];
	return "Unknown";
}

void __init eml_dump_memory_map(void)
{
	efi_memory_desc_t *map;
	unsigned long map_size, desc_size, map_key;
	u32 desc_version;
	efi_status_t status;
	unsigned long i;
	u64 total_mem = 0, total_free = 0;

	map_size = 0;
	status = eml_systab->boot_services->get_memory_map(
		&map_size, NULL, &map_key, &desc_size, &desc_version);

	map_size += desc_size * 4;
	status = eml_systab->boot_services->allocate_pool(
		EFI_LOADER_DATA, map_size, (void **)&map);
	if (status != EFI_SUCCESS) {
		pr_err("EML: failed to allocate memory map buffer\n");
		return;
	}

	status = eml_systab->boot_services->get_memory_map(
		&map_size, map, &map_key, &desc_size, &desc_version);
	if (status != EFI_SUCCESS) {
		pr_err("EML: GetMemoryMap failed: 0x%lx\n",
		       (unsigned long)status);
		eml_systab->boot_services->free_pool(map);
		return;
	}

	pr_info("EML: EFI memory map (%lu entries):\n", map_size / desc_size);

	for (i = 0; i < map_size; i += desc_size) {
		efi_memory_desc_t *desc = (void *)map + i;
		u64 size = desc->num_pages << EFI_PAGE_SHIFT;

		pr_info("  %016llx-%016llx %-14s %4llu MB\n", desc->phys_addr,
			desc->phys_addr + size - 1,
			eml_mem_type_name(desc->type), size >> 20);

		total_mem += size;
		if (desc->type == EFI_CONVENTIONAL_MEMORY)
			total_free += size;
	}

	pr_info("EML: total: %llu MB, free conventional: %llu MB\n",
		total_mem >> 20, total_free >> 20);

	eml_systab->boot_services->free_pool(map);
}

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
