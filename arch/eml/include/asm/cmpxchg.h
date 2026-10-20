/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_CMPXCHG_H
#define _ASM_EML_CMPXCHG_H

/*
 * EML is single-threaded (!SMP) so we can use simple non-atomic
 * implementations. We use GCC builtins for correctness.
 */

#define __xchg(ptr, new, size)            \
	({                                \
		__typeof__(*(ptr)) __old; \
		__old = *(ptr);           \
		*(ptr) = (new);           \
		__old;                    \
	})

#define arch_xchg(ptr, new)                           \
	({                                            \
		__typeof__(*(ptr)) __new = (new);     \
		__xchg((ptr), __new, sizeof(*(ptr))); \
	})

#define arch_cmpxchg(ptr, old, new)                              \
	({                                                       \
		__typeof__(*(ptr)) __old = (old), __new = (new); \
		__typeof__(*(ptr)) __prev = *(ptr);              \
		if (__prev == __old)                             \
			*(ptr) = __new;                          \
		__prev;                                          \
	})

#define arch_cmpxchg64(ptr, old, new) arch_cmpxchg((ptr), (old), (new))

#define arch_cmpxchg_local(ptr, old, new) arch_cmpxchg((ptr), (old), (new))
#define arch_cmpxchg64_local(ptr, old, new) arch_cmpxchg((ptr), (old), (new))
#define arch_xchg_local(ptr, new) arch_xchg((ptr), (new))

#endif /* _ASM_EML_CMPXCHG_H */
