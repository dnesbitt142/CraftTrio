/* IEEE-754 rounding helpers for the Open Watcom Wasm3 compatibility layer. */
#ifndef WC_OW_IEEE_ROUND_H
#define WC_OW_IEEE_ROUND_H

#include <stdint.h>

typedef enum {
    WC_FP_TRUNC, WC_FP_FLOOR, WC_FP_CEIL, WC_FP_NEAREST
} WcFpRoundMode;

static inline double wc_fp_round(double x, WcFpRoundMode mode)
{
    const uint64_t sign_mask = UINT64_C(0x8000000000000000);
    const uint64_t infinity = UINT64_C(0x7ff0000000000000);
    const uint64_t one = UINT64_C(0x3ff0000000000000);
    const uint64_t half = UINT64_C(0x3fe0000000000000);
    union { double value; uint64_t bits; } number;
    uint64_t sign, magnitude, unit, fraction, rounded;
    int exponent;

    number.value = x;
    sign = number.bits & sign_mask;
    magnitude = number.bits & ~sign_mask;
    if (magnitude > infinity) {
        /* Rounding a NaN produces an arithmetic (quiet) NaN. */
        number.bits |= UINT64_C(0x0008000000000000);
        return number.value;
    }
    exponent = (int)(magnitude >> 52) - 1023;
    if (!magnitude || exponent >= 52)
        return number.value;
    if (exponent < 0) {
        if ((mode == WC_FP_FLOOR && sign) ||
            (mode == WC_FP_CEIL && !sign) ||
            (mode == WC_FP_NEAREST && magnitude > half)) {
            number.bits = sign | one;
        } else {
            number.bits = sign;
        }
        return number.value;
    }

    unit = UINT64_C(1) << (52 - exponent);
    fraction = magnitude & (unit - 1);
    rounded = magnitude - fraction;
    if (fraction && ((mode == WC_FP_FLOOR && sign) ||
                     (mode == WC_FP_CEIL && !sign) ||
                     (mode == WC_FP_NEAREST &&
                      (fraction > unit / 2 ||
                       (fraction == unit / 2 && (rounded & unit)))))) {
        rounded += unit;
    }
    number.bits = sign | rounded;
    return number.value;
}

#endif
