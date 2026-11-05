/* Isolated candidate used before installing the same definitions in helpers.c. */
#include <stdint.h>
int __isnan(double value) {
    union { double value; uint64_t bits; } number;
    number.value=value;
    return (number.bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000);
}
int __isnanf(float value) {
    union { float value; uint32_t bits; } number;
    number.value=value;
    return (number.bits & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000);
}
