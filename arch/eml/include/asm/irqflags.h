/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_IRQFLAGS_H
#define _ASM_EML_IRQFLAGS_H

#include <linux/types.h>

extern unsigned long eml_irq_flags;

static inline unsigned long arch_local_save_flags(void)
{
	return eml_irq_flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	eml_irq_flags = flags;
}

static inline void arch_local_irq_disable(void)
{
	eml_irq_flags = 0;
}

static inline void arch_local_irq_enable(void)
{
	eml_irq_flags = 1;
}

static inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	return flags == 0;
}

static inline bool arch_irqs_disabled(void)
{
	return eml_irq_flags == 0;
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = eml_irq_flags;

	eml_irq_flags = 0;
	return flags;
}

#endif /* _ASM_EML_IRQFLAGS_H */
