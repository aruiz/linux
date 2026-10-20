// SPDX-License-Identifier: GPL-2.0
#include <linux/syscalls.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <asm/syscalls.h>

asmlinkage long sys_rt_sigreturn(struct pt_regs *regs)
{
	return -ENOSYS;
}

asmlinkage long sys_clone(unsigned long flags, unsigned long newsp,
			  int __user *parent_tidptr, int __user *child_tidptr,
			  unsigned long tls)
{
	return kernel_clone(&(struct kernel_clone_args){
		.flags = (lower_32_bits(flags) & ~CSIGNAL),
		.pidfd = parent_tidptr,
		.child_tid = child_tidptr,
		.parent_tid = parent_tidptr,
		.exit_signal = (lower_32_bits(flags) & CSIGNAL),
		.stack = newsp,
		.tls = tls,
	});
}

SYSCALL_DEFINE6(mmap, unsigned long, addr, unsigned long, len, unsigned long,
		prot, unsigned long, flags, unsigned long, fd, unsigned long,
		off)
{
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, off >> PAGE_SHIFT);
}

#undef __SYSCALL
#define __SYSCALL(nr, call) [nr] = (call),
#define __SYSCALL_WITH_COMPAT(nr, native, compat) __SYSCALL(nr, native)

void *sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = sys_ni_syscall,
#include <uapi/asm-generic/unistd.h>
};
