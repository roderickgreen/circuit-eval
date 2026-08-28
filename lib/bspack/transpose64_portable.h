#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* The 64x64 bit-matrix transpose, portable vector core -- every non-arm64
 * target.  Follows the selection block in bspack.c.
 *
 * bs_transpose_64x64 transposes in place (new[k].bit[l] = old[l].bit[k]),
 * run over the BS_LANES u64 lanes of each row at once so that BS_LANES
 * independent matrices transpose simultaneously.
 *
 * The algorithm is recursive halving (Hacker's Delight 7-3): six stages of
 * 32 masked swaps, where stage j exchanges bit j of the row index with bit
 * j of the bit index.  Different j touch different bits, so the stages
 * commute and may be grouped into passes.  Here stages {32,16,8} share one
 * pass over stride-8 row sets and stages {4,2,1} one pass over contiguous
 * 8-row blocks; each pass holds every row it touches in registers, so the
 * array is read and written twice rather than six times.
 *
 * Also here: the masks, the swap macros and LADDER421, which pack_portable.h
 * builds its own pass schedules from. */

#if BS_LANES == 8
#define BCAST(C) {C, C, C, C, C, C, C, C}
#elif BS_LANES == 4
#define BCAST(C) {C, C, C, C}
#elif BS_LANES == 2
#define BCAST(C) {C, C}
#else
#define BCAST(C) {C}
#endif
static const bs_word ZEROW = BCAST(0);
static const bs_word M32 = BCAST(0x00000000FFFFFFFFull);
static const bs_word M16 = BCAST(0x0000FFFF0000FFFFull);
static const bs_word M8  = BCAST(0x00FF00FF00FF00FFull);
static const bs_word M4  = BCAST(0x0F0F0F0F0F0F0F0Full);
static const bs_word M2  = BCAST(0x3333333333333333ull);
static const bs_word M1  = BCAST(0x5555555555555555ull);

/* one swap: exchange the M-masked bits of lo shifted down by J with the
 * corresponding bits of hi.  The shifted value is fenced (BS_PIN, from
 * bspack.c): where a row entering a ladder is a known zero on some paths
 * (pack_portable.h's unpack, whose rows beyond nout are zero), an
 * unfenced swap folds on those paths and the compiler then emits the
 * rest of the ladder once per path; the fence keeps one copy. */
#define SW(J, M, lo, hi)                            \
    do {                                            \
        bs_word s_ = lo >> (J);                     \
        BS_PIN(s_);                                 \
        bs_word t_ = (s_ ^ hi) & M;                 \
        hi ^= t_;                                   \
        lo ^= t_ << (J);                            \
    } while (0)

/* the same swap where hi is known to be zero: four operations, not six.
 * pack_portable.h's bs_untranspose meets this case throughout, because
 * only ceil(nout/8) of the plane octets it starts from hold anything. */
#define SW0(J, M, lo, hi)                           \
    do {                                            \
        bs_word s_ = lo >> (J);                     \
        BS_PIN(s_);                                 \
        bs_word t_ = s_ & M;                        \
        hi = t_;                                    \
        lo ^= t_ << (J);                            \
    } while (0)

/* stages 4,2,1 over eight rows held in registers -- the second pass below
 * and pack_portable.h's first (pack_avx2.h carries its own intrinsic
 * spelling of the same twelve swaps) */
#define LADDER421(r0, r1, r2, r3, r4, r5, r6, r7)   \
    do {                                            \
        SW(4, M4, r0, r4); SW(4, M4, r1, r5);       \
        SW(4, M4, r2, r6); SW(4, M4, r3, r7);       \
        SW(2, M2, r0, r2); SW(2, M2, r1, r3);       \
        SW(2, M2, r4, r6); SW(2, M2, r5, r7);       \
        SW(1, M1, r0, r1); SW(1, M1, r2, r3);       \
        SW(1, M1, r4, r5); SW(1, M1, r6, r7);       \
    } while (0)

void bs_transpose_64x64(bs_word rows[64])
{
    /* stages 32,16,8 over stride-8 row sets */
    for (int k0 = 0; k0 < 8; k0++) {
        bs_word r0 = rows[k0],      r1 = rows[k0 + 8],  r2 = rows[k0 + 16],
                r3 = rows[k0 + 24], r4 = rows[k0 + 32], r5 = rows[k0 + 40],
                r6 = rows[k0 + 48], r7 = rows[k0 + 56];
        SW(32, M32, r0, r4); SW(32, M32, r1, r5);
        SW(32, M32, r2, r6); SW(32, M32, r3, r7);
        SW(16, M16, r0, r2); SW(16, M16, r1, r3);
        SW(16, M16, r4, r6); SW(16, M16, r5, r7);
        SW(8,  M8,  r0, r1); SW(8,  M8,  r2, r3);
        SW(8,  M8,  r4, r5); SW(8,  M8,  r6, r7);
        rows[k0]      = r0; rows[k0 + 8]  = r1;
        rows[k0 + 16] = r2; rows[k0 + 24] = r3;
        rows[k0 + 32] = r4; rows[k0 + 40] = r5;
        rows[k0 + 48] = r6; rows[k0 + 56] = r7;
    }

    /* stages 4,2,1 over contiguous 8-row blocks */
    for (int g = 0; g < 64; g += 8) {
        bs_word r0 = rows[g],     r1 = rows[g + 1], r2 = rows[g + 2],
                r3 = rows[g + 3], r4 = rows[g + 4], r5 = rows[g + 5],
                r6 = rows[g + 6], r7 = rows[g + 7];
        LADDER421(r0, r1, r2, r3, r4, r5, r6, r7);
        rows[g]     = r0; rows[g + 1] = r1; rows[g + 2] = r2;
        rows[g + 3] = r3; rows[g + 4] = r4; rows[g + 5] = r5;
        rows[g + 6] = r6; rows[g + 7] = r7;
    }
}
