/*
 * gcc-check.c - a miscompile that the SDK's GCC flags must avoid.
 *
 * MSYS2's i686 GCC 15.2 at -O2 compiles entry() below as
 *     addl 16(%eax), %eax        ; low half: object + p->off
 *     adcl 20(%eax), %edx        ; high half, from the *changed* %eax
 * reading the upper half of p->off from the wrong address.  GCC 15's late
 * combine pass does this; -fno-late-combine-instructions avoids it.  It
 * corrupted the HSA runtime's kernel entry addresses (kernel object + code
 * offset), and the same compiler builds all of LLVM, so prepare-sdk.sh
 * builds and runs this with the flags it uses before anything else.
 *
 * Exit status 0 when the compiler gets it right.
 */
#include <stdio.h>
#include <string.h>

struct desc {
    unsigned a, b, c, d;
    long long off;
};

__attribute__((noinline)) unsigned long long entry(unsigned long long object)
{
    const struct desc *p = (const struct desc *)(unsigned long)object;

    return object + (unsigned long long)p->off;
}

int main(void)
{
    static struct desc d[8];
    unsigned long long want, got;
    unsigned junk = 0x7e080203;

    d[0].off = 0x40;
    /* whatever sits where the miscompiled code reads */
    memcpy((char *)&d[0] + 0x40 + 20, &junk, sizeof(junk));
    want = (unsigned long)&d[0] + 0x40;
    got = entry((unsigned long)&d[0]);
    if (got != want) {
        printf("miscompiled: %08lx:%08lx instead of %08lx:%08lx\n",
               (unsigned long)(got >> 32), (unsigned long)got,
               (unsigned long)(want >> 32), (unsigned long)want);
        return 1;
    }
    return 0;
}
