/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_EML_SIGCONTEXT_H
#define _ASM_EML_SIGCONTEXT_H

/* EML has no signals or real userspace; minimal stub */

struct sigcontext {
	unsigned long fault_address;
};

#endif /* _ASM_EML_SIGCONTEXT_H */
