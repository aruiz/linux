// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/ptrace.h>
#include <linux/delay.h>
#include <linux/stacktrace.h>
#include <linux/mm.h>
#include <linux/reboot.h>
#include <asm/setup.h>

struct task_struct *eml_current_task = &init_task;

static char eml_command_line[COMMAND_LINE_SIZE] __initdata;

void __init setup_arch(char **cmdline_p)
{
	*cmdline_p = eml_command_line;
}

void __init init_IRQ(void)
{
}

void __init time_init(void)
{
}

void machine_halt(void)
{
	for (;;)
		asm volatile("hlt");
}

void machine_power_off(void)
{
	machine_halt();
}

void machine_restart(char *cmd)
{
	machine_halt();
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	return -ENOSYS;
}

void flush_thread(void)
{
}

struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	return prev;
}

void show_regs(struct pt_regs *regs)
{
}

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
}

long arch_ptrace(struct task_struct *child, long request, unsigned long addr,
		 unsigned long data)
{
	return -EIO;
}

void ptrace_disable(struct task_struct *child)
{
}

void __delay(unsigned long loops)
{
	while (loops--)
		cpu_relax();
}

void __const_udelay(unsigned long xloops)
{
	__delay(xloops >> 12);
}

void save_stack_trace(struct stack_trace *trace)
{
}

void arch_zone_limits_init(unsigned long *max_zone_pfn)
{
}

int syscall_get_arch(struct task_struct *task)
{
	return 0;
}

static int eml_cpuinfo_show(struct seq_file *m, void *v)
{
	seq_puts(m, "processor\t: 0\nmodel name\t: EFI Mode Linux\n");
	return 0;
}

static void *eml_cpuinfo_start(struct seq_file *m, loff_t *pos)
{
	return *pos ? NULL : (void *)1;
}

static void *eml_cpuinfo_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void eml_cpuinfo_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start = eml_cpuinfo_start,
	.next = eml_cpuinfo_next,
	.stop = eml_cpuinfo_stop,
	.show = eml_cpuinfo_show,
};
