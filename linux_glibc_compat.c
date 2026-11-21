/*
 * linux_glibc_compat.c -- keep the Linux build runnable on glibc 2.35+.
 *
 * Built on a newer system, the binary would ask for glibc 2.38: its headers
 * quietly route strtol, sscanf and friends to new C23 entry points
 * (__isoc23_*), fmod gained a new symbol version, and arc4random arrived in
 * 2.36. None of what this program does needs the new behaviour, so the new
 * names are defined here on top of the long-standing functions. The binary then
 * runs on SteamOS 3.5+, Ubuntu 22.04+, Debian 12+ and similar.
 *
 * Compiled as plain C11 (no _GNU_SOURCE), so the calls below are the classic
 * symbols and not redirected again.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#if defined(__linux__) && defined(__GLIBC__)

long __isoc23_strtol(const char* s, char** e, int b) { return strtol(s, e, b); }
long long __isoc23_strtoll(const char* s, char** e, int b) { return strtoll(s, e, b); }
unsigned long __isoc23_strtoul(const char* s, char** e, int b) { return strtoul(s, e, b); }
unsigned long long __isoc23_strtoull(const char* s, char** e, int b) { return strtoull(s, e, b); }

int __isoc23_sscanf(const char* s, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    const int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
int __isoc23_fscanf(FILE* f, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    const int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}
int __isoc23_vsscanf(const char* s, const char* fmt, va_list ap) { return vsscanf(s, fmt, ap); }
int __isoc23_vfscanf(FILE* f, const char* fmt, va_list ap) { return vfscanf(f, fmt, ap); }

/* fmod: the x86-64 baseline version, not the GLIBC_2.38 one. */
extern double fp_glibc_fmod(double, double);
__asm__(".symver fp_glibc_fmod, fmod@GLIBC_2.2.5");
double fmod(double x, double y) { return fp_glibc_fmod(x, y); }

/* arc4random (2.36): from getrandom (2.25), which never fails for small sizes
 * once the system's pool is up. */
extern ssize_t getrandom(void* buf, size_t len, unsigned flags);
uint32_t arc4random(void)
{
    uint32_t v = 0;
    while (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) { }
    return v;
}
#endif
