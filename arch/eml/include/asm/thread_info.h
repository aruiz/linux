/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_THREAD_INFO_H
#define _ASM_EML_THREAD_INFO_H

#include <asm/page.h>

/*
 * With THREAD_INFO_IN_TASK, thread_info is embedded in task_struct
 * and we only need a minimal definition here.
 */

#define THREAD_SIZE (4 * PAGE_SIZE)
#define THREAD_SIZE_ORDER 2

#ifndef __ASSEMBLY__

struct thread_info {
	unsigned long flags;
	int preempt_count;
};

#define INIT_THREAD_INFO(tsk) \
	{                     \
		.flags = 0,   \
	}

#define TIF_SIGPENDING 0
#define TIF_NEED_RESCHED 1
#define TIF_NOTIFY_RESUME 2
#define TIF_NOTIFY_SIGNAL 3
#define TIF_MEMDIE 4
#define TIF_SYSCALL_TRACE 5
#define TIF_SYSCALL_AUDIT 6
#define TIF_SECCOMP 7

#define _TIF_SIGPENDING (1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED (1 << TIF_NEED_RESCHED)
#define _TIF_NOTIFY_RESUME (1 << TIF_NOTIFY_RESUME)
#define _TIF_NOTIFY_SIGNAL (1 << TIF_NOTIFY_SIGNAL)
#define _TIF_SYSCALL_TRACE (1 << TIF_SYSCALL_TRACE)

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_EML_THREAD_INFO_H */
