/* Timestamp counter for the secondary cycles/hand figures: rdtsc on
 * x86-64, cntvct_el0 on aarch64.  cntvct is a fixed-frequency timer
 * (24 MHz or 1 GHz depending on the core generation), NOT core cycles,
 * so on arm the figure is only comparable to itself run-to-run;
 * TSC_UNIT names what was actually counted.  The ns/hand headline in
 * every bench comes from clock_gettime either way. */
#ifndef TSC_H
#define TSC_H

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline unsigned long long tsc(void) { return __rdtsc(); }
#define TSC_UNIT "rdtsc-cycles"
#elif defined(__aarch64__)
static inline unsigned long long tsc(void)
{
    unsigned long long v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#define TSC_UNIT "cntvct-ticks"
#else
static inline unsigned long long tsc(void) { return 0; }
#define TSC_UNIT "no-tsc"
#endif

#endif
