/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_PTRACE_H
#define _ASM_EML_PTRACE_H

#include <uapi/asm/ptrace.h>

#define user_mode(regs) (0)
#define kernel_mode(regs) (1)
#define instruction_pointer(regs) ((regs)->rip)
#define user_stack_pointer(regs) ((regs)->rsp)
#define profile_pc(regs) instruction_pointer(regs)

#endif /* _ASM_EML_PTRACE_H */
