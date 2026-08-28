#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* The 64x64 bit-matrix transpose, arm64/NEON core.  Same ladder and schedule as
 * transpose64_portable.h -- read that file's comment first.  The pack API
 * on arm64 is pack_neon.h, which shares the macros below; this file's
 * own entry point is bs_transpose_64x64. */

#if BS_BATCH != 128 && BS_BATCH != 256
#error "the arm64/NEON core supports 128 or 256 lanes only (no wider vectors there)"
#endif

/* ==== arm64/NEON core ====================================================
 *
 * Same ladder; two NEON facts change the instruction economics:
 *
 *  - A stage-J masked swap for J in {8,16,32} is exactly the trn1/trn2
 *    permute pair at J-bit element width, and the stride-64 gather is
 *    trn1/trn2 on u64 lanes: 2 instructions per pair where the shift+mask
 *    form compiles to ~6.  (clang does not find this on its own -- the
 *    generic core compiles to zero trn instructions.)
 *  - Stage 4 is a byte-wise sri/sli shift-insert pair, per byte:
 *        hi' = (hi & 0xF0) | (lo >> 4)      = SRI(hi, lo, #4)
 *        lo' = (lo & 0x0F) | (hi << 4)      = SLI(lo, hi, #4)
 *    valid only because the byte is exactly 2J wide.  Stages 2/1 have no
 *    4/2-bit element type, so they run as ushr+shl+2x bsl bit-selects
 *    (4 real ops vs 6; the bsl operand movs rename away on Apple cores).
 *
 * The BS_LANES u64 lanes of a bs_word are processed as BS_LANES/2
 * independent uint64x2_t halves (half h = hands 128h..128h+127) -- one half
 * at BS_BATCH=128, two at BS_BATCH=256 -- keeping the ladder schedule
 * spill-free regardless of width.  Plane layout in memory is unchanged from
 * the generic core.
 */
#include <arm_neon.h>

#define TRN8(lo, hi)                                                     \
    do {                                                                 \
        uint8x16_t a_ = vreinterpretq_u8_u64(lo);                        \
        uint8x16_t b_ = vreinterpretq_u8_u64(hi);                        \
        lo = vreinterpretq_u64_u8(vtrn1q_u8(a_, b_));                    \
        hi = vreinterpretq_u64_u8(vtrn2q_u8(a_, b_));                    \
    } while (0)

#define TRN16(lo, hi)                                                    \
    do {                                                                 \
        uint16x8_t a_ = vreinterpretq_u16_u64(lo);                       \
        uint16x8_t b_ = vreinterpretq_u16_u64(hi);                       \
        lo = vreinterpretq_u64_u16(vtrn1q_u16(a_, b_));                  \
        hi = vreinterpretq_u64_u16(vtrn2q_u16(a_, b_));                  \
    } while (0)

#define TRN32(lo, hi)                                                    \
    do {                                                                 \
        uint32x4_t a_ = vreinterpretq_u32_u64(lo);                       \
        uint32x4_t b_ = vreinterpretq_u32_u64(hi);                       \
        lo = vreinterpretq_u64_u32(vtrn1q_u32(a_, b_));                  \
        hi = vreinterpretq_u64_u32(vtrn2q_u32(a_, b_));                  \
    } while (0)

#define SRISLI4(lo, hi)                                                  \
    do {                                                                 \
        uint8x16_t lo_ = vreinterpretq_u8_u64(lo);                       \
        uint8x16_t hi_ = vreinterpretq_u8_u64(hi);                       \
        hi = vreinterpretq_u64_u8(vsriq_n_u8(hi_, lo_, 4));              \
        lo = vreinterpretq_u64_u8(vsliq_n_u8(lo_, hi_, 4));              \
    } while (0)

#define SWBSL(J, M, MN, lo, hi)                                          \
    do {                                                                 \
        uint64x2_t t_ = vshrq_n_u64(lo, J);                              \
        uint64x2_t u_ = vshlq_n_u64(hi, J);                              \
        hi = vbslq_u64(M, t_, hi);                                       \
        lo = vbslq_u64(MN, u_, lo);                                      \
    } while (0)

/* stages 4,2,1 over an 8-row block (needs C2/C2N/C1/C1N in scope) */
#define LADDER421(r)                                                     \
    do {                                                                 \
        SRISLI4(r[0], r[4]); SRISLI4(r[1], r[5]);                        \
        SRISLI4(r[2], r[6]); SRISLI4(r[3], r[7]);                        \
        SWBSL(2, C2, C2N, r[0], r[2]); SWBSL(2, C2, C2N, r[1], r[3]);    \
        SWBSL(2, C2, C2N, r[4], r[6]); SWBSL(2, C2, C2N, r[5], r[7]);    \
        SWBSL(1, C1, C1N, r[0], r[1]); SWBSL(1, C1, C1N, r[2], r[3]);    \
        SWBSL(1, C1, C1N, r[4], r[5]); SWBSL(1, C1, C1N, r[6], r[7]);    \
    } while (0)

#define DECL_MASKS                                                       \
    const uint64x2_t C2  = vdupq_n_u64(0x3333333333333333ull);           \
    const uint64x2_t C2N = vdupq_n_u64(0xCCCCCCCCCCCCCCCCull);           \
    const uint64x2_t C1  = vdupq_n_u64(0x5555555555555555ull);           \
    const uint64x2_t C1N = vdupq_n_u64(0xAAAAAAAAAAAAAAAAull)

/* Same stage32 = 0 form as the generic core (drop the 64-bit stage, here
 * the u32 trn pair, and the four 32x32 submatrices transpose independently
 * in place); unused on arm64 -- pack_neon.h fuses its own unpack -- but
 * kept so the two cores share a shape.
 *
 * always_inline, as in the generic core: stage32 is a literal at the call
 * site and the stage must fold away rather than be branched around.  The
 * r[] load/store loops carry the same #pragma GCC unroll 8 as the x86
 * cores -- r[] must end in registers -- so neither depends on the
 * compiler's own inlining and complete-unroll heuristics.  (clang does
 * both unaided: its output is byte-identical with and without.) */
static inline __attribute__((always_inline)) void
ladder_64x64(bs_word rows[64], int stage32)
{
    DECL_MASKS;

    for (int h = 0; h < BS_LANES / 2; h++) {
        uint64_t *p = (uint64_t *)rows + 2 * h;   /* half h of each row */

        /* all-permute stages 32,16,8 over stride-8 row sets */
        for (int k0 = 0; k0 < 8; k0++) {
            uint64x2_t r[8];
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                r[i] = vld1q_u64(p + BS_LANES * (k0 + 8 * i));
            if (stage32) {
                TRN32(r[0], r[4]); TRN32(r[1], r[5]);
                TRN32(r[2], r[6]); TRN32(r[3], r[7]);
            }
            TRN16(r[0], r[2]); TRN16(r[1], r[3]);
            TRN16(r[4], r[6]); TRN16(r[5], r[7]);
            TRN8(r[0], r[1]); TRN8(r[2], r[3]);
            TRN8(r[4], r[5]); TRN8(r[6], r[7]);
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                vst1q_u64(p + BS_LANES * (k0 + 8 * i), r[i]);
        }

        /* stages 4,2,1 over contiguous 8-row blocks */
        for (int g = 0; g < 64; g += 8) {
            uint64x2_t r[8];
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                r[i] = vld1q_u64(p + BS_LANES * (g + i));
            LADDER421(r);
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                vst1q_u64(p + BS_LANES * (g + i), r[i]);
        }
    }
}

void bs_transpose_64x64(bs_word rows[64]) { ladder_64x64(rows, 1); }
