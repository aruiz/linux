/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_IRQFLAGS_H
#define _ASM_EML_IRQFLAGS_H

#include <linux/types.h>

static inline unsigned long arch_local_save_flags(void)
{
	return 0;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
}

static inline void arch_local_irq_disable(void)
{
}

static inline void arch_local_irq_enable(void)
{
}

static inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	return false;
}

static inline bool arch_irqs_disabled(void)
{
	return false;
}

static inline unsigned long arch_local_irq_save(void)
{
	return 0;
}

#endif /* _ASM_EML_IRQFLAGS_H */
