#define __SIZEOF_POINTER__ 4
#define __BYTE_ORDER__ 1234
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __STDC_NO_VLA__ 1
#define M3_HAS_TAIL_CALL 0
#define __attribute__(x)
#include <stdint.h>
#include <math.h>
#include "ow-ieee-round.h"
#define trunc(x) wc_fp_round((x), WC_FP_TRUNC)
#define floor(x) wc_fp_round((x), WC_FP_FLOOR)
#define ceil(x) wc_fp_round((x), WC_FP_CEIL)
#define rint(x) wc_fp_round((x), WC_FP_NEAREST)
static inline int ow_clz64(uint64_t v) {
    int n = 0;
    if (!v) return 64;
    while ((v >> 63) == 0) { v <<= 1; ++n; }
    return n;
}
static inline int ow_ctz64(uint64_t v) {
    int n = 0;
    if (!v) return 64;
    while ((v & 1) == 0) { v >>= 1; ++n; }
    return n;
}
static inline int ow_pop64(uint64_t v) {
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}
static inline float ow_copysignf(float x, float y) {
    union { uint32_t i; float f; } a, b;
    a.f = x;
    b.f = y;
    a.i = (a.i & 0x7fffffffUL) | (b.i & 0x80000000UL);
    return a.f;
}
#define __builtin_clz(x) (ow_clz64((uint32_t)(x)) - 32)
#define __builtin_clzll(x) ow_clz64(x)
#define __builtin_ctz(x) ow_ctz64((uint32_t)(x))
#define __builtin_ctzll(x) ow_ctz64(x)
#define __builtin_popcount(x) ow_pop64((uint32_t)(x))
#define __builtin_popcountll(x) ow_pop64(x)
#define copysignf(x,y) ow_copysignf(x,y)
#define fabsf(x) ow_copysignf(x,0.0f)
#define ceilf(x) ((float)ceil(x))
#define floorf(x) ((float)floor(x))
#define truncf(x) ((float)trunc(x))
#define sqrtf(x) ((float)sqrt(x))
#define rintf(x) ((float)rint(x))
#include <stddef.h>
static inline size_t ow_strnlen(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) ++n;
    return n;
}
#define strnlen(s,n) ow_strnlen(s,n)
