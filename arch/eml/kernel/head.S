/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PE/COFF header for EFI Mode Linux
 *
 * This file produces a valid PE/COFF header so that UEFI firmware can
 * load the kernel as an EFI application.  The entry point jumps to
 * eml_efi_entry() defined in eml_main.c.
 */

#include <linux/pe.h>

	.section	".head", "a"
	.globl		eml_pe_header
eml_pe_header:
.Ldoshdr:
	.long		IMAGE_DOS_SIGNATURE		/* "MZ" */
	.zero		56				/* pad to offset 0x3c */
	.long		.Lpehdr - .Ldoshdr		/* PE header offset */

.Lpehdr:
	.long		IMAGE_NT_SIGNATURE		/* "PE\0\0" */
	/* COFF header */
	.short		IMAGE_FILE_MACHINE_AMD64
	.short		.Lsection_count
	.long		0				/* TimeDateStamp */
	.long		0				/* PointerToSymbolTable */
	.long		0				/* NumberOfSymbols */
	.short		.Lsection_table - .Loptional_header
	.short		IMAGE_FILE_DEBUG_STRIPPED | \
			IMAGE_FILE_EXECUTABLE_IMAGE | \
			IMAGE_FILE_LINE_NUMS_STRIPPED

.Loptional_header:
	.short		IMAGE_NT_OPTIONAL_HDR64_MAGIC	/* PE32+ */
	.byte		0, 0				/* linker version */
	.long		_etext - .Lefi_header_end	/* SizeOfCode */
	.long		__data_size			/* SizeOfInitializedData */
	.long		0				/* SizeOfUninitializedData */
	.long		eml_pe_entry - .Ldoshdr		/* AddressOfEntryPoint */
	.long		.Lefi_header_end - .Ldoshdr	/* BaseOfCode */

	.quad		0				/* ImageBase */
	.long		4096				/* SectionAlignment */
	.long		512				/* FileAlignment */
	.short		0, 0				/* OS version */
	.short		0, 0				/* Image version */
	.short		0, 0				/* Subsystem version */
	.long		0				/* Win32VersionValue */

	/*
	 * SizeOfImage: set to _end + extra space for the .reloc section.
	 * The build script will update this and the .reloc section header
	 * after generating relocations.
	 */
	.globl		eml_pe_size_of_image
eml_pe_size_of_image:
	.long		_end - .Ldoshdr			/* SizeOfImage */

	.long		.Lefi_header_end - .Ldoshdr	/* SizeOfHeaders */
	.long		0				/* CheckSum */
	.short		IMAGE_SUBSYSTEM_EFI_APPLICATION
	.short		IMAGE_DLLCHARACTERISTICS_NX_COMPAT
	.quad		0, 0, 0, 0			/* stack/heap sizes */
	.long		0				/* LoaderFlags */
	.long		6				/* NumberOfRvaAndSizes */

	.quad		0				/* ExportTable */
	.quad		0				/* ImportTable */
	.quad		0				/* ResourceTable */
	.quad		0				/* ExceptionTable */
	.quad		0				/* CertificationTable */

	/*
	 * BaseRelocationTable: RVA and size filled in by the build script.
	 */
	.globl		eml_pe_reloc_dir
eml_pe_reloc_dir:
	.long		0				/* BaseRelocationTable RVA */
	.long		0				/* BaseRelocationTable Size */

.Lsection_table:
	/*
	 * Section 1: .text (code + rodata)
	 */
	.ascii		".text\0\0\0"
	.long		_etext - .Lefi_header_end	/* VirtualSize */
	.long		.Lefi_header_end - .Ldoshdr	/* VirtualAddress */
	.long		_etext - .Lefi_header_end	/* SizeOfRawData */
	.long		.Lefi_header_end - .Ldoshdr	/* PointerToRawData */
	.long		0, 0				/* relocations, line numbers */
	.short		0, 0
	.long		IMAGE_SCN_CNT_CODE | \
			IMAGE_SCN_MEM_READ | \
			IMAGE_SCN_MEM_EXECUTE

	/*
	 * Section 2: .data (writable data + bss)
	 */
	.ascii		".data\0\0\0"
	.long		__data_size			/* VirtualSize */
	.long		_data - .Ldoshdr		/* VirtualAddress */
	.long		__data_rawsize			/* SizeOfRawData */
	.long		_data - .Ldoshdr		/* PointerToRawData */
	.long		0, 0
	.short		0, 0
	.long		IMAGE_SCN_CNT_INITIALIZED_DATA | \
			IMAGE_SCN_MEM_READ | \
			IMAGE_SCN_MEM_WRITE

	/*
	 * Section 3: .reloc (PE base relocations, filled by build script)
	 * The build script patches VirtualSize, SizeOfRawData, VirtualAddress,
	 * and PointerToRawData after generating relocations.
	 */
	.globl		eml_pe_reloc_section
eml_pe_reloc_section:
	.ascii		".reloc\0\0"
	.long		0				/* VirtualSize */
	.long		0				/* VirtualAddress */
	.long		0				/* SizeOfRawData */
	.long		0				/* PointerToRawData */
	.long		0, 0
	.short		0, 0
	.long		IMAGE_SCN_CNT_INITIALIZED_DATA | \
			IMAGE_SCN_MEM_READ | \
			IMAGE_SCN_MEM_DISCARDABLE

	.set		.Lsection_count, (. - .Lsection_table) / 40

	.p2align	12
.Lefi_header_end:

/*
 * PE entry point trampoline — called by UEFI firmware.
 * Arguments (MS x64 ABI):
 *   %rcx = EFI_HANDLE  ImageHandle
 *   %rdx = EFI_SYSTEM_TABLE *SystemTable
 */
	.section	".init.text", "ax"
	.globl		eml_pe_entry
eml_pe_entry:
	subq		$40, %rsp		/* shadow space for MS ABI */
	/* Convert MS x64 ABI to System V: rcx→rdi, rdx→rsi */
	movq		%rcx, %rdi
	movq		%rdx, %rsi
	call		eml_efi_entry
	addq		$40, %rsp
1:	hlt
	jmp		1b

	.text

#include <generated/asm-offsets.h>

/*
 * __switch_to(prev, next) — context switch between kernel tasks.
 *
 * copy_thread() builds a matching stack frame for new threads so that
 * popping callee-saved registers + ret lands at ret_from_fork with
 * rbx = fn and r12 = fn_arg.
 */
	.globl		__switch_to
__switch_to:
	pushq		%rbp
	pushq		%rbx
	pushq		%r12
	pushq		%r13
	pushq		%r14
	pushq		%r15

	movq		%rsp, TASK_THREAD_RSP(%rdi)
	movq		TASK_THREAD_RSP(%rsi), %rsp

	movq		%rsi, eml_current_task(%rip)
	movq		%rdi, %rax

	popq		%r15
	popq		%r14
	popq		%r13
	popq		%r12
	popq		%rbx
	popq		%rbp
	retq

/*
 * ret_from_fork — new task entry point.
 * Reached via retq from __switch_to when copy_thread()'s stack frame
 * is unwound.  %rax = prev, rbx = fn (or 0), r12 = fn_arg.
 */
	.globl		ret_from_fork
ret_from_fork:
	movq		%rax, %rdi
	call		schedule_tail
	testq		%rbx, %rbx
	jz		1f
	movq		%r12, %rdi
	call		*%rbx
1:	hlt
	jmp		1b
