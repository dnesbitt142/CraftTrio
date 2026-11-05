#include <stdio.h>
#include <stdint.h>
extern int qa_classify64(uint64_t);
extern int qa_classify32(uint32_t);
static void float_mode(void);
#pragma aux float_mode = "sub esp,4" "fnclex" "fnstcw word ptr [esp]" \
    "and word ptr [esp],0f0ffh" "or word ptr [esp],023fh" \
    "fldcw word ptr [esp]" "add esp,4" modify exact [];
static const uint64_t bits64[]={
    UINT64_C(0),UINT64_C(0x8000000000000000),UINT64_C(1),UINT64_C(0x8000000000000001),
    UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff),
    UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff0000000000001),UINT64_C(0xfff0000000000001),
    UINT64_C(0x7ff8000000000000),UINT64_C(0xfff8000000000000),UINT64_C(0x7fffffffffffffff),UINT64_C(0xffffffffffffffff)};
static const uint32_t bits32[]={
    0,0x80000000UL,1,0x80000001UL,0x3f800000UL,0xbf800000UL,0x7f7fffffUL,0xff7fffffUL,
    0x7f800000UL,0xff800000UL,0x7f800001UL,0xff800001UL,0x7fc00000UL,0xffc00000UL,0x7fffffffUL,0xffffffffUL};
int main(void) {
    unsigned i;
    for(i=0;i<sizeof(bits64)/sizeof(bits64[0]);i++) {
        float_mode();
        if(!!qa_classify64(bits64[i]) != (i>=10)) { printf("FAIL f64 classification %u\n",i);return 1; }
    }
    for(i=0;i<sizeof(bits32)/sizeof(bits32[0]);i++) {
        float_mode();
        if(!!qa_classify32(bits32[i]) != (i>=10)) { printf("FAIL f32 classification %u\n",i);return 1; }
    }
    puts("PASS 32 isolated classifier cases: zeros, subnormals, finite, infinities, qNaNs, sNaNs, both signs; x87=0x023f");
    return 0;
}
