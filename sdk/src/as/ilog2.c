#include "compiler.h"
#include "aslib.h"

#define ROUND(v, a, w)                 \
    do                                 \
    {                                  \
        if (v & (((1 << w) - 1) << w)) \
        {                              \
            a += w;                    \
            v >>= w;                   \
        }                              \
    } while (0)

#if defined(__GNUC__) && defined(__x86_64__)

int ilog2_32(uint32_t v)
{
    int n;

    __asm__("bsrl %1,%0"
            : "=r"(n)
            : "rm"(v), "0"(0));
    return n;
}

#elif defined(__GNUC__) && defined(__i386__)

int ilog2_32(uint32_t v)
{
    int n;

    __asm__("bsrl %1,%0 ; jnz 1f ; xorl %0,%0\n"
            "1:"
            : "=&r"(n)
            : "rm"(v));
    return n;
}

#elif defined(HAVE_GNUC_4)

int ilog2_32(uint32_t v)
{
    if (!v)
        return 0;

    return __builtin_clz(v) ^ 31;
}

#else

int ilog2_32(uint32_t v)
{
    int p = 0;

    ROUND(v, p, 16);
    ROUND(v, p, 8);
    ROUND(v, p, 4);
    ROUND(v, p, 2);
    ROUND(v, p, 1);

    return p;
}

#endif

#if defined(__GNUC__) && defined(__x86_64__)

int ilog2_64(uint64_t v)
{
    uint64_t n;

    __asm__("bsrq %1,%0"
            : "=r"(n)
            : "rm"(v), "0"(UINT64_C(0)));
    return n;
}

#elif defined(HAVE_GNUC_4)

int ilog2_64(uint64_t v)
{
    if (!v)
        return 0;

    return __builtin_clzll(v) ^ 63;
}

#else

int ilog2_64(uint64_t vv)
{
    int p = 0;
    uint32_t v;

    v = vv >> 32;
    if (v)
        p += 32;
    else
        v = vv;

    ROUND(v, p, 16);
    ROUND(v, p, 8);
    ROUND(v, p, 4);
    ROUND(v, p, 2);
    ROUND(v, p, 1);

    return p;
}

#endif

/*
 * v == 0 ? 0 : is_power2(x) ? ilog2_X(v) : -1
 */
int alignlog2_32(uint32_t v)
{
    if (unlikely(v & (v - 1)))
        return -1; /* invalid alignment */

    return ilog2_32(v);
}

int alignlog2_64(uint64_t v)
{
    if (unlikely(v & (v - 1)))
        return -1; /* invalid alignment */

    return ilog2_64(v);
}
