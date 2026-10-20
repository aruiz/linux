/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_ATOMIC_H
#define _ASM_EML_ATOMIC_H

/*
 * EML is !SMP so atomics are trivial non-atomic operations.
 * 32-bit operations come from asm-generic/atomic.h,
 * 64-bit operations are defined here since we're a 64-bit arch.
 */

#include <asm/cmpxchg.h>
#include <asm/barrier.h>

/* 32-bit atomic ops - use the generic cmpxchg-based ones */
#include <asm-generic/atomic.h>

#define ATOMIC64_INIT(i) { (i) }

/*
 * 64-bit atomic ops - trivial for !SMP 64-bit arch.
 * Must be macros so atomic-arch-fallback.h can detect them
 * via #if defined(arch_atomic64_*).
 */
#define arch_atomic64_read(v) READ_ONCE((v)->counter)
#define arch_atomic64_set(v, i) WRITE_ONCE((v)->counter, (i))

#define arch_atomic64_add(i, v) ((void)((v)->counter += (i)))
#define arch_atomic64_sub(i, v) ((void)((v)->counter -= (i)))

#define arch_atomic64_add_return(i, v) ((v)->counter += (i))
#define arch_atomic64_sub_return(i, v) ((v)->counter -= (i))

#define arch_atomic64_fetch_add(i, v)     \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter += (i);      \
		__old;                    \
	})

#define arch_atomic64_fetch_sub(i, v)     \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter -= (i);      \
		__old;                    \
	})

#define arch_atomic64_and(i, v) ((void)((v)->counter &= (i)))
#define arch_atomic64_or(i, v) ((void)((v)->counter |= (i)))
#define arch_atomic64_xor(i, v) ((void)((v)->counter ^= (i)))

#define arch_atomic64_fetch_and(i, v)     \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter &= (i);      \
		__old;                    \
	})

#define arch_atomic64_fetch_or(i, v)      \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter |= (i);      \
		__old;                    \
	})

#define arch_atomic64_fetch_xor(i, v)     \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter ^= (i);      \
		__old;                    \
	})

#define arch_atomic64_xchg(v, new)        \
	({                                \
		s64 __old = (v)->counter; \
		(v)->counter = (new);     \
		__old;                    \
	})

#define arch_atomic64_cmpxchg(v, old, new)    \
	({                                    \
		s64 __prev = (v)->counter;    \
		if (__prev == (s64)(old))     \
			(v)->counter = (new); \
		__prev;                       \
	})

#endif /* _ASM_EML_ATOMIC_H */
