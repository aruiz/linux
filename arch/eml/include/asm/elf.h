/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_ELF_H
#define _ASM_EML_ELF_H

/*
 * ELF definitions for EML. The kernel never loads ELF binaries in EML
 * (no real userspace) but some headers reference these types.
 */

#include <asm/ptrace.h>

#define ELF_ARCH EM_X86_64
#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB

typedef unsigned long elf_greg_t;
#define ELF_NGREG 32
typedef elf_greg_t elf_gregset_t[ELF_NGREG];
typedef struct {
} elf_fpregset_t;

#define elf_check_arch(x) \
	((x)->e_machine == EM_X86_64 && (x)->e_ident[EI_CLASS] == ELFCLASS64)
#define ELF_EXEC_PAGESIZE PAGE_SIZE
#define ELF_ET_DYN_BASE (TASK_SIZE / 3 * 2)
#define ELF_PLATFORM "eml"
#define ELF_HWCAP (0)
#define ELF_PLAT_INIT(regs, load_addr)

#define SET_PERSONALITY(ex) \
	do {                \
	} while (0)

#endif /* _ASM_EML_ELF_H */
