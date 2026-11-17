/*
 * nocrt.c - the two C runtime routines the compiler may call implicitly.
 * Neither driver links against a C runtime.
 */
#include <stddef.h>
#include <intrin.h>

#pragma function(memset, memcpy)

void *__cdecl memset(void *dst, int c, size_t n)
{
    __stosb((unsigned char *)dst, (unsigned char)c, n);
    return dst;
}

void *__cdecl memcpy(void *dst, const void *src, size_t n)
{
    __movsb((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}
