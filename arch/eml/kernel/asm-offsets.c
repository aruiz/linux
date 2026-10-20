// SPDX-License-Identifier: GPL-2.0

#include <linux/kbuild.h>
#include <linux/sched.h>

int main(void)
{
	DEFINE(TASK_THREAD_RSP, offsetof(struct task_struct, thread.rsp));
	DEFINE(TASK_THREAD_RIP, offsetof(struct task_struct, thread.rip));
	return 0;
}
