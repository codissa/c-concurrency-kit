#ifndef CK_COMMON_H
#define CK_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* Assumed destructive-interference size. 64 bytes on x86-64 and most ARM cores. */
#ifndef CK_CACHELINE
#define CK_CACHELINE 64
#endif

/* Hint to the CPU that we are in a spin-wait loop. Reduces power draw and,
 * on hyper-threaded cores, yields execution resources to the sibling thread. */
static inline void ck_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Round up to the next power of two (returns 0 on overflow). */
static inline size_t ck_next_pow2(size_t v)
{
    if (v < 2)
        return 2;
    v--;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        v |= v >> shift;
    return v + 1;
}

/* Round n up to a multiple of align (align must be a power of two). */
static inline size_t ck_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

#endif /* CK_COMMON_H */
