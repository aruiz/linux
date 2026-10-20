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

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define X86_EFLAGS_IF 0x200

static inline void wrmsr(u32 msr, u64 val)
{
	asm volatile("wrmsr"
		     :
		     : "c"(msr), "a"((u32)val), "d"((u32)(val >> 32)));
}

struct task_struct *eml_current_task = &init_task;
unsigned long eml_irq_flags = 1;

extern char eml_cmdline[];
extern void eml_dump_memory_map(void);
extern void eml_boot_kernel(void);

void __init setup_arch(char **cmdline_p)
{
	eml_setup_memory();
	eml_setup_early_console();
	eml_dump_memory_map();
	eml_parse_command_line();
	eml_boot_kernel();
	strscpy(boot_command_line, eml_cmdline, COMMAND_LINE_SIZE);
	*cmdline_p = boot_command_line;
}

extern void eml_syscall_entry(void);

static void __init eml_setup_syscall(void)
{
	/*
	 * Set up SYSCALL/SYSRET MSRs.
	 * STAR[47:32] = kernel CS selector for SYSCALL
	 * STAR[63:48] = user CS selector base for SYSRET (unused — we
	 *               don't call SYSRETQ, but the MSR must be set)
	 * We use the UEFI-provided segments (CS is typically 0x38 in OVMF).
	 * Since EML runs entirely in ring 0, SYSCALL stays at CPL 0.
	 */
	u16 cs;
	u64 star;

	asm volatile("mov %%cs, %0" : "=r"(cs));
	star = ((u64)cs << 32) | ((u64)cs << 48);
	wrmsr(MSR_STAR, star);
	wrmsr(MSR_LSTAR, (u64)eml_syscall_entry);
	wrmsr(MSR_SFMASK, X86_EFLAGS_IF);

	pr_info("EML: SYSCALL entry at %px, CS=0x%x\n", eml_syscall_entry, cs);
}

void eml_return_to_user(struct pt_regs *regs)
{
	asm volatile("movq %0, %%rsp\n"
		     "popq %%r15\n"
		     "popq %%r14\n"
		     "popq %%r13\n"
		     "popq %%r12\n"
		     "popq %%rbp\n"
		     "popq %%rbx\n"
		     "popq %%r11\n"
		     "popq %%r10\n"
		     "popq %%r9\n"
		     "popq %%r8\n"
		     "popq %%rax\n"
		     "popq %%rcx\n"
		     "popq %%rdx\n"
		     "popq %%rsi\n"
		     "popq %%rdi\n"
		     "addq $8, %%rsp\n" /* skip orig_rax */
		     "popq %%rcx\n" /* rip -> rcx for jmp */
		     "addq $8, %%rsp\n" /* skip cs */
		     "popfq\n" /* restore eflags */
		     "popq %%rsp\n" /* restore user stack */
		     "jmpq *%%rcx\n"
		     :
		     : "r"(regs)
		     : "memory");
	__builtin_unreachable();
}

void __init init_IRQ(void)
{
	eml_setup_syscall();
}

void __init time_init(void)
{
	eml_setup_timer();
}

extern void eml_process_ticks(void);
extern void eml_kbd_poll(void);

void arch_cpu_idle(void)
{
	eml_process_ticks();
	eml_kbd_poll();
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

static void eml_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	asm volatile("cpuid"
		     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		     : "a"(leaf), "c"(0));
}

static int eml_cpuinfo_show(struct seq_file *m, void *v)
{
	u32 eax, ebx, ecx, edx;
	u32 brand[12];
	u32 family, model, stepping;
	int i;

	seq_puts(m, "processor\t: 0\n");

	/* Get vendor string */
	eml_cpuid(0, &eax, &ebx, &ecx, &edx);
	seq_printf(m, "vendor_id\t: %.4s%.4s%.4s\n", (char *)&ebx, (char *)&edx,
		   (char *)&ecx);

	/* Get family/model/stepping */
	eml_cpuid(1, &eax, &ebx, &ecx, &edx);
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;
	stepping = eax & 0xf;
	if (family == 0xf)
		family += (eax >> 20) & 0xff;
	if (family >= 6)
		model += ((eax >> 16) & 0xf) << 4;

	seq_printf(m, "cpu family\t: %u\n", family);
	seq_printf(m, "model\t\t: %u\n", model);
	seq_printf(m, "stepping\t: %u\n", stepping);

	/* Get brand string (leaves 0x80000002-0x80000004) */
	eml_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
	if (eax >= 0x80000004) {
		for (i = 0; i < 3; i++)
			eml_cpuid(0x80000002 + i, &brand[i * 4],
				  &brand[i * 4 + 1], &brand[i * 4 + 2],
				  &brand[i * 4 + 3]);
		seq_printf(m, "model name\t: %s\n", (char *)brand);
	} else {
		seq_puts(m, "model name\t: EFI Mode Linux\n");
	}

	seq_puts(m, "flags\t\t: eml efi nommu\n");
	seq_puts(m, "bogomips\t: ");
	seq_printf(m, "%lu.%02lu\n", loops_per_jiffy / (500000 / HZ),
		   (loops_per_jiffy / (5000 / HZ)) % 100);

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
