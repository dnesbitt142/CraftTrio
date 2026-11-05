#include <stdint.h>
extern int __isnan(double);
extern int __isnanf(float);
int qa_classify64(uint64_t bits) {
    union { double value; uint64_t bits; } number;
    number.bits=bits;
    return __isnan(number.value);
}
int qa_classify32(uint32_t bits) {
    union { float value; uint32_t bits; } number;
    number.bits=bits;
    return __isnanf(number.value);
}
