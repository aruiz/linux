/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_SWITCH_TO_H
#define _ASM_EML_SWITCH_TO_H

extern struct task_struct *__switch_to(struct task_struct *prev,
				       struct task_struct *next);

#define switch_to(prev, next, last)                   \
	do {                                          \
		(last) = __switch_to((prev), (next)); \
	} while (0)

#endif /* _ASM_EML_SWITCH_TO_H */
