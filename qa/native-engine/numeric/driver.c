/* QA only: integer stack ABI around generated floating-point operations. */
#include <stdio.h>
#include <stdint.h>
#include "numeric.h"
#include "wasm-rt-impl.h"
#include "wasm-rt-exceptions.h"

static void float_mode(void);
#pragma aux float_mode = "sub esp,4" "fnclex" "fnstcw word ptr [esp]" \
    "and word ptr [esp],0f0ffh" "or word ptr [esp],023fh" \
    "fldcw word ptr [esp]" "add esp,4" modify exact [];

typedef u64 (*Numeric)(w2c_numeric *, u64, u64);
typedef struct { const char *name; Numeric call; u64 a, b, expected; unsigned mode; } Test;
/* mode 0 exact bits; 1 quiet f64 NaN; 2 quiet f32 NaN; otherwise trap+3. */
#include "cases.h"
#ifdef __OS2__
#define QA_RUNTIME "native OS/2 ABI"
#else
#define QA_RUNTIME "QEMU Linux ABI"
#endif

int main(void) {
    w2c_numeric instance;
    wasm_rt_trap_t trap;
    unsigned i, checks=0, traps=0;
    u64 result;
    float_mode();wasm_rt_init();
    trap=wasm_rt_impl_try();
    if(trap) { printf("FAIL instantiate trap=%u\n",(unsigned)trap);return 1; }
    wasm2c_numeric_instantiate(&instance);
    for(i=0;i<sizeof(tests)/sizeof(tests[0]);i++) {
        float_mode();
        trap=wasm_rt_impl_try();
        if(trap) {
            if(tests[i].mode<3 || (unsigned)trap!=tests[i].mode-3) {
                printf("FAIL %u %s trap=%u mode=%u\n",i,tests[i].name,(unsigned)trap,tests[i].mode);return 1;
            }
            traps++;checks++;continue;
        }
        result=tests[i].call(&instance,tests[i].a,tests[i].b);
        if(tests[i].mode>=3 ||
           (tests[i].mode==0 && result!=tests[i].expected) ||
           (tests[i].mode==1 && (result&UINT64_C(0x7ff8000000000000))!=UINT64_C(0x7ff8000000000000)) ||
           (tests[i].mode==2 && ((result>>32)!=0 || (result&UINT64_C(0x7fc00000))!=UINT64_C(0x7fc00000)))) {
            printf("FAIL %u %s result=%08lx%08lx expected=%08lx%08lx mode=%u\n",i,tests[i].name,
                   (unsigned long)(result>>32),(unsigned long)result,
                   (unsigned long)(tests[i].expected>>32),(unsigned long)tests[i].expected,tests[i].mode);return 1;
        }
        checks++;
    }
    wasm2c_numeric_free(&instance);wasm_rt_free();
    printf("PASS %u numeric/ABI checks including %u expected traps; x87=0x023f; " QA_RUNTIME "\n",checks,traps);
    return 0;
}
