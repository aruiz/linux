// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/seq_file.h>
#include <linux/ptrace.h>
#include <linux/delay.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/mmzone.h>
#include <linux/reboot.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/eml_efi.h>

struct task_struct *eml_current_task = &init_task;
unsigned long eml_irq_flags = 1;

extern char eml_cmdline[];

void __init setup_arch(char **cmdline_p)
{
	eml_setup_memory();
	eml_setup_early_console();
	eml_parse_command_line();
	*cmdline_p = eml_cmdline;
}

void __init init_IRQ(void)
{
}

void __init time_init(void)
{
	eml_setup_timer();
}

void machine_halt(void)
{
	for (;;)
		asm volatile("hlt");
}

void machine_power_off(void)
{
	if (eml_systab && eml_systab->runtime_services)
		eml_systab->runtime_services->reset_system(
			EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
	machine_halt();
}

void machine_restart(char *cmd)
{
	if (eml_systab && eml_systab->runtime_services)
		eml_systab->runtime_services->reset_system(
			EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
	machine_halt();
}

struct eml_switch_frame {
	unsigned long r15, r14, r13, r12, rbx, rbp;
	unsigned long ret_addr;
};

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct pt_regs *childregs = task_pt_regs(p);
	struct eml_switch_frame *frame;

	memset(childregs, 0, sizeof(*childregs));

	if (unlikely(args->idle))
		return 0;

	frame = ((struct eml_switch_frame *)childregs) - 1;
	memset(frame, 0, sizeof(*frame));
	frame->ret_addr = (unsigned long)ret_from_fork;

	if (args->fn) {
		frame->rbx = (unsigned long)args->fn;
		frame->r12 = (unsigned long)args->fn_arg;
	} else {
		*childregs = *current_pt_regs();
		childregs->rax = 0;
	}

	p->thread.rsp = (unsigned long)frame;
	return 0;
}

void flush_thread(void)
{
}

void show_regs(struct pt_regs *regs)
{
	if (!regs)
		return;

	pr_emerg("RIP: %016lx\n", regs->rip);
	pr_emerg("RSP: %016lx EFLAGS: %08lx\n", regs->rsp, regs->eflags);
	pr_emerg("RAX: %016lx RBX: %016lx RCX: %016lx\n", regs->rax, regs->rbx,
		 regs->rcx);
	pr_emerg("RDX: %016lx RSI: %016lx RDI: %016lx\n", regs->rdx, regs->rsi,
		 regs->rdi);
	pr_emerg("RBP: %016lx R08: %016lx R09: %016lx\n", regs->rbp, regs->r8,
		 regs->r9);
	pr_emerg("R10: %016lx R11: %016lx R12: %016lx\n", regs->r10, regs->r11,
		 regs->r12);
	pr_emerg("R13: %016lx R14: %016lx R15: %016lx\n", regs->r13, regs->r14,
		 regs->r15);
}

static bool eml_in_kernel_text(unsigned long addr)
{
	return addr >= (unsigned long)_text && addr < (unsigned long)_end;
}

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	unsigned long *stack;
	int i;

	if (!sp) {
		if (task && task != current)
			sp = (unsigned long *)task->thread.rsp;
		else
			asm volatile("mov %%rsp, %0" : "=r"(sp));
	}

	printk("%sStack:\n", loglvl);
	stack = sp;
	for (i = 0; i < 24; i++)
		printk("%s %016lx\n", loglvl, stack[i]);

	printk("%sCall Trace:\n", loglvl);
	for (i = 0; i < 64; i++) {
		if (eml_in_kernel_text(stack[i]))
			printk("%s [<%016lx>]\n", loglvl, stack[i]);
	}
}

long arch_ptrace(struct task_struct *child, long request, unsigned long addr,
		 unsigned long data)
{
	return -EIO;
}

void ptrace_disable(struct task_struct *child)
{
}

void save_stack_trace(struct stack_trace *trace)
{
}

void arch_zone_limits_init(unsigned long *max_zone_pfn)
{
	max_zone_pfn[ZONE_NORMAL] = max_low_pfn;
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
