/* The lane word every bitsliced object is built on: BS_BATCH hands per
 * word, one per bit lane, as a GCC vector of BS_BATCH/64 uint64 lanes.
 * Included by bsapi.h and by every generated circuit_*.c.
 *
 * The batch width is an architectural constant, not a tunable, and the
 * default is the target's vector register width: 512 where the compile
 * target has AVX-512 (one gate = one zmm op), 128 on NEON (one q
 * register), 256 elsewhere.  A word wider than a register is split by the
 * compiler into as many registers, which divides the usable register file
 * by the same factor and spills harder; the circuits are straight-line
 * code with hundreds of values live at the peak, so that costs more than
 * the wider word gains.
 *
 * Every array in the API is sized by it, and every object linked into one
 * binary must agree on it.  It can be pinned on the compiler command line
 * (-DBS_BATCH=64/128/256/512).  A width the target has no register for
 * runs through split vector ops; 64 is plain uint64_t words and needs no
 * vector unit at all.  Those are testing and portability paths, not fast
 * ones.  The build system reads the default back out of this header
 * rather than repeating the rule, so there is one place to change it. */
#pragma once

#ifndef BS_BATCH
# if defined(__AVX512F__)
#  define BS_BATCH 512
# elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#  define BS_BATCH 128
# else
#  define BS_BATCH 256
# endif
#endif
#define BS_LANES (BS_BATCH / 64)   /* u64 lanes per plane word */

typedef unsigned long long bs_word __attribute__((vector_size(BS_BATCH / 8)));

/* A bs_word is BS_BATCH / 8 bytes and must be aligned to its size; the
 * circuits store through aligned vector ops.  Stack and static objects
 * get that alignment from the type.  Heap storage does not: malloc
 * aligns to 16, so a bs_word array on the heap must come from
 * aligned_alloc(BS_ALIGN, size) or equivalent, or a store faults. */
#define BS_ALIGN (BS_BATCH / 8)

/* T3(imm, a, b, c, expr): the 3-input gate with truth table imm over
 * (a, b, c) -- bit 4a+2b+c of imm is the value at (a, b, c) -- as one
 * ternary-logic instruction where the target has it at this width, else
 * as expr, the same function written in & | ~.  The instruction
 * overwrites its first operand; the generator puts the operand at its
 * last use there.  Emitted by flow/codegen/cpu/bitslice.py. */
#if defined(__AVX512F__) && BS_BATCH == 512
# include <immintrin.h>
# define T3(imm, a, b, c, expr) \
    ((bs_word)_mm512_ternarylogic_epi64((__m512i)(a), (__m512i)(b), \
                                        (__m512i)(c), (imm)))
#else
# define T3(imm, a, b, c, expr) (expr)
#endif

/* BS_KEEP(p): an empty asm that reads p, so p stays live up to it.  The
 * generated circuits load every input into a local at the top and place
 * this just before the output stores.  Without it the input pointer dies
 * after the last load, and a compiler that spills a value loaded from
 * memory by re-reading that memory only does so while the memory stays
 * unchanged and the pointer it was read through stays live until the
 * value dies; with it every input the allocator spills is re-read from
 * the input array instead of stored to the frame and reloaded from
 * there.  No instruction is emitted. */
#define BS_KEEP(p) __asm__("" :: "r"(p))

