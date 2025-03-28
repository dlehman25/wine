//===-- aeabi_uldivmod.S - EABI uldivmod implementation -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "../assembly.h"

// struct { uint64_t quot, uint64_t rem}
//        __aeabi_uldivmod(uint64_t numerator, uint64_t denominator) {
//   uint64_t rem, quot;
//   quot = __udivmoddi4(numerator, denominator, &rem);
//   return {quot, rem};
// }

#if defined(_WIN32)
#define __aeabi_uldivmod __rt_udiv64
#endif

        .syntax unified
        .p2align 2
DEFINE_COMPILERRT_FUNCTION(__aeabi_uldivmod)
        push	{r6, lr}
        sub	sp, sp, #16
        add	r6, sp, #8
        str	r6, [sp]
#if defined(_WIN32)
        movs    r6, r0
        movs    r0, r2
        movs    r2, r6
        movs    r6, r1
        movs    r1, r3
        movs    r3, r6
#endif
        bl	SYMBOL_NAME(__udivmoddi4)
        ldr	r2, [sp, #8]
        ldr	r3, [sp, #12]
        add	sp, sp, #16
        pop	{r6, pc}
END_COMPILERRT_FUNCTION(__aeabi_uldivmod)

NO_EXEC_STACK_DIRECTIVE
