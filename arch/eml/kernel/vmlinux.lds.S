/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linker script for EFI Mode Linux (EML)
 *
 * Lays out sections for a PE/COFF UEFI application. The PE header is
 * embedded in the .head section by head.S; objcopy -O binary produces
 * the final .efi image.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(eml_pe_header)

SECTIONS
{
	. = 0;

	.head : ALIGN(4096) {
		*(.head)
	}

	.text : ALIGN(4096) {
		_stext = .;
		*(.text .text.*)
		*(.init.text .init.text.*)
		_etext = ALIGN(4096);
		. = _etext;
	}

	.rodata : ALIGN(4096) {
		_srodata = .;
		*(.rodata .rodata.*)
		*(.init.rodata .init.rodata.*)
		*(.srodata .srodata.*)
		_erodata = ALIGN(4096);
		. = _erodata;
	}

	.data : ALIGN(4096) {
		_sdata = .;
		_data = .;
		*(.data .data.*)
		*(.init.data .init.data.*)
		_edata = ALIGN(512);
		. = _edata;
	}

	.bss : ALIGN(4096) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		__bss_stop = ALIGN(512);
		. = __bss_stop;
		_end = .;
	}

	/DISCARD/ : {
		*(.discard .discard.*)
		*(.modinfo .init.modinfo)
		*(.comment)
		*(.note.*)
		*(.eh_frame*)
		*(.got .got.plt)
	}
}

/* Symbols used by the PE header in head.S */
PROVIDE(__text_size = ABSOLUTE(_etext - _stext));
PROVIDE(__rodata_size = ABSOLUTE(_erodata - _srodata));
PROVIDE(__data_rawsize = ABSOLUTE(_edata - _data));
PROVIDE(__data_size = ABSOLUTE(_end - _data));
