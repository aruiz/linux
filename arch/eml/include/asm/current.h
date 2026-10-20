/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_CURRENT_H
#define _ASM_EML_CURRENT_H

#ifndef __ASSEMBLY__

struct task_struct;

/*
 * EML: simple global current pointer.
 * With !SMP there is only one task running at a time.
 */
extern struct task_struct *eml_current_task;

#define current eml_current_task

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_EML_CURRENT_H */
