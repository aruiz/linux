/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linker script for EFI Mode Linux (EML)
 *
 * PE/COFF layout with two sections:
 *   .text — code, rodata, init (read-only/execute)
 *   .data — writable data and BSS
 *
 * The PE headers are embedded in .head by head.S.
 */

#include <asm-generic/vmlinux.lds.h>
#include <asm/page.h>
#include <asm/thread_info.h>

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(eml_pe_header)

jiffies = jiffies_64;

SECTIONS
{
	. = 0;

	.head : ALIGN(4096) {
		*(.head)
	}

	/* PE .text section: code + rodata + init */
	_text = .;
	.text : ALIGN(4096) {
		_stext = .;
		HEAD_TEXT
		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
		NOINSTR_TEXT
		*(.fixup)
		*(.gnu.warning)
	}

	/* Init text */
	. = ALIGN(PAGE_SIZE);
	__init_begin = .;
	_sinittext = .;
	.init.text : {
		INIT_TEXT
	}
	_einittext = .;

	/* Init data, setup, initcalls, console initcalls */
	.init.data : {
		INIT_DATA
		INIT_SETUP(16)
		INIT_CALLS
		CON_INITCALL
		INIT_RAM_FS
	}
	. = ALIGN(PAGE_SIZE);
	__init_end = .;

	/* Read-only data */
	RO_DATA(PAGE_SIZE)

	EXCEPTION_TABLE(16)

	NOTES

	_etext = ALIGN(4096);
	. = _etext;

	/* PE .data section: writable data + BSS */
	. = ALIGN(4096);
	_sdata = .;
	_data = .;

	RW_DATA(64, PAGE_SIZE, THREAD_SIZE)

	_edata = .;

	BSS_SECTION(PAGE_SIZE, PAGE_SIZE, PAGE_SIZE)
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	ELF_DETAILS

	/DISCARD/ : {
		*(.discard .discard.*)
		*(.modinfo .init.modinfo)
		*(.comment)
		*(.eh_frame*)
		*(.got .got.plt)
	}
}

/* Symbols used by the PE header in head.S */
PROVIDE(__data_rawsize = ABSOLUTE(_edata - _data));
PROVIDE(__data_size = ABSOLUTE(_end - _data));
