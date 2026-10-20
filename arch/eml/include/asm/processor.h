/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_PROCESSOR_H
#define _ASM_EML_PROCESSOR_H

#include <asm/ptrace.h>

/*
 * EML has no real userspace; TASK_SIZE is meaningless but must be defined.
 * Use a large value that fits the UEFI address space.
 */
#define TASK_SIZE (0x00007fffffffffffUL)
#define TASK_UNMAPPED_BASE (TASK_SIZE / 3)

#define STACK_TOP TASK_SIZE
#define STACK_TOP_MAX TASK_SIZE

struct thread_struct {
	unsigned long rsp;
	unsigned long rip;
};

#define INIT_THREAD \
	{           \
	}

#define task_pt_regs(task) \
	((struct pt_regs *)((unsigned long)(task)->stack + THREAD_SIZE) - 1)
#define current_pt_regs() task_pt_regs(current)

extern void ret_from_fork(void);

#define start_thread(regs, new_ip, new_sp)      \
	do {                                    \
		memset(regs, 0, sizeof(*regs)); \
		(regs)->rip = (new_ip);         \
		(regs)->rsp = (new_sp);         \
		(regs)->cs = 0;                 \
		(regs)->ss = 0;                 \
		(regs)->eflags = 0x200;         \
	} while (0)

static inline void cpu_relax(void)
{
	asm volatile("rep; nop" ::: "memory");
}

#define current_text_addr()                            \
	({                                             \
		void *_addr;                           \
		asm("lea 0(%%rip), %0" : "=r"(_addr)); \
		_addr;                                 \
	})

static inline unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}

#define KSTK_EIP(task) 0
#define KSTK_ESP(task) 0

#endif /* _ASM_EML_PROCESSOR_H */
