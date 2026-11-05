/* i386 compiler helpers for the GCC objects linked into the OS/2 executable. */
#include <stdint.h>

#include "ow-ieee-round.h"

/* GCC emits these classifiers when wasm2c is built with -fsignaling-nans. */
int __isnan(double value)
{
    union { double value; uint64_t bits; } number;
    number.value = value;
    return (number.bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000);
}

int __isnanf(float value)
{
    union { float value; uint32_t bits; } number;
    number.value = value;
    return (number.bits & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000);
}

int __clzdi2(uint64_t value)
{
    uint32_t upper = (uint32_t)(value >> 32);
    if (upper) return __builtin_clz(upper);
    return value ? 32 + __builtin_clz((uint32_t)value) : 64;
}

int __ctzdi2(uint64_t value)
{
    uint32_t lower = (uint32_t)value;
    if (lower) return __builtin_ctz(lower);
    return value ? 32 + __builtin_ctz((uint32_t)(value >> 32)) : 64;
}

int __popcountdi2(uint64_t value)
{
    int count = 0;
    while (value) {
        value &= value - 1;
        ++count;
    }
    return count;
}

/* Integer division without a dependency on the Linux libgcc runtime. */
uint64_t __udivmoddi4(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    uint64_t quotient = 0;
    uint64_t rest = 0;
    unsigned int bit;
    if (divisor && dividend <= UINT32_MAX) {
        uint32_t small = (uint32_t)dividend;
        if (divisor > small) {
            if (remainder) *remainder = small;
            return 0;
        }
        if (remainder) *remainder = small % (uint32_t)divisor;
        return small / (uint32_t)divisor;
    }
    for (bit = 0; bit < 64; ++bit) {
        uint32_t carry = (uint32_t)(rest >> 63);
        rest = (rest << 1) | (dividend >> 63);
        dividend <<= 1;
        quotient <<= 1;
        if (carry || rest >= divisor) {
            rest -= divisor;
            quotient |= 1;
        }
    }
    if (remainder) *remainder = rest;
    return quotient;
}

uint64_t __udivdi3(uint64_t dividend, uint64_t divisor)
{
    return __udivmoddi4(dividend, divisor, 0);
}

uint64_t __umoddi3(uint64_t dividend, uint64_t divisor)
{
    uint64_t remainder;
    __udivmoddi4(dividend, divisor, &remainder);
    return remainder;
}

static uint64_t magnitude(int64_t value)
{
    return value < 0 ? -(uint64_t)value : (uint64_t)value;
}

int64_t __divdi3(int64_t dividend, int64_t divisor)
{
    uint64_t quotient = __udivmoddi4(magnitude(dividend), magnitude(divisor), 0);
    if ((dividend < 0) != (divisor < 0)) quotient = -quotient;
    return (int64_t)quotient;
}

int64_t __moddi3(int64_t dividend, int64_t divisor)
{
    uint64_t remainder;
    __udivmoddi4(magnitude(dividend), magnitude(divisor), &remainder);
    if (dividend < 0) remainder = -remainder;
    return (int64_t)remainder;
}

double ceil(double value) { return wc_fp_round(value, WC_FP_CEIL); }
float ceilf(float value) { return (float)wc_fp_round(value, WC_FP_CEIL); }
double floor(double value) { return wc_fp_round(value, WC_FP_FLOOR); }
float floorf(float value) { return (float)wc_fp_round(value, WC_FP_FLOOR); }
double trunc(double value) { return wc_fp_round(value, WC_FP_TRUNC); }
float truncf(float value) { return (float)wc_fp_round(value, WC_FP_TRUNC); }
double nearbyint(double value) { return wc_fp_round(value, WC_FP_NEAREST); }
float nearbyintf(float value) { return (float)wc_fp_round(value, WC_FP_NEAREST); }

double sqrt(double value)
{
    double result;
    __asm__("fsqrt" : "=t"(result) : "0"(value));
    return result;
}

float sqrtf(float value)
{
    float result;
    __asm__("fsqrt" : "=t"(result) : "0"(value));
    return result;
}

double copysign(double value, double sign)
{
    union { double value; uint64_t bits; } magnitude, source;
    const uint64_t sign_mask = UINT64_C(1) << 63;
    magnitude.value = value;
    source.value = sign;
    magnitude.bits = (magnitude.bits & ~sign_mask) | (source.bits & sign_mask);
    return magnitude.value;
}

float copysignf(float value, float sign)
{
    union { float value; uint32_t bits; } magnitude, source;
    const uint32_t sign_mask = UINT32_C(1) << 31;
    magnitude.value = value;
    source.value = sign;
    magnitude.bits = (magnitude.bits & ~sign_mask) | (source.bits & sign_mask);
    return magnitude.value;
}

int __popcountsi2(uint32_t value)
{
    value -= (value >> 1) & UINT32_C(0x55555555);
    value = (value & UINT32_C(0x33333333)) + ((value >> 2) & UINT32_C(0x33333333));
    value = (value + (value >> 4)) & UINT32_C(0x0f0f0f0f);
    return (int)((value * UINT32_C(0x01010101)) >> 24);
}
