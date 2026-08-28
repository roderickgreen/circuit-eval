#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* ==== pack API, AVX2 (4 lanes) =========================================
 *
 * Follows transpose64_portable.h.  Implements bs_transpose / bs_transpose_map / bs_untranspose in
 * intrinsics.
 *
 * Instruction selection here assumes a core that issues two shuffles per
 * cycle against four logic ops, runs lane-crossing permutes (vperm2i128,
 * vpermq) at half the shuffle rate, and treats a 128-bit broadcast load as
 * a pure load while a 128-bit insert from memory takes a shuffle slot.  So
 * shuffle slots go only to byte-granular work no logic op can do, the
 * batch gather is built from broadcast loads and vpblendd, and each
 * 128-bit lane crossing is placed on the output side, one per store.  A
 * core with a narrower shuffle unit relative to its vector width wants
 * those three choices re-checked.
 *
 * Two building blocks, both lane-local (each u64 lane of a ymm is its own
 * 64x64 matrix, as everywhere in bspack):
 *
 *  LADDER421Q stages 4/2/1 over eight rows: an 8x8 bit transpose of every
 *             byte-aligned tile, new[i] byte t bit l = old[l] byte t bit
 *             i.  Six ops per swap, two of them shifts.
 *  bytetr8    an 8x8 byte transpose across eight rows, per lane, as the
 *             three-stage vpunpck tree (24 shuffles).  Its natural output
 *             pairs lane h with lane h+2 in one register; the pack spends
 *             8 vpunpck{l,h}qdq to re-pair them, the unpack instead defers
 *             the vperm2i128 to its final stores.
 *
 * pack (bs_transpose / bs_transpose_map): two passes.  Pass 1 gathers 8
 * consecutive rows and runs LADDER421Q; the gather is broadcast loads +
 * vpblendd + vpunpckqdq, 8 shuffle slots per 8 rows.  Pass 2 takes each
 * stride-8 row set through stages 8/16/32, which together are exactly the
 * per-lane byte transpose of that set, so bytetr8 performs them.  Pass 2
 * is unrolled on a literal k0, so the map variant's idx < 52 tests fold
 * and planes 52..63 -- with the shuffles feeding only them -- are not
 * emitted.
 *
 * unpack (bs_untranspose): byte tiles.  The output is a u32 per hand with
 * nout <= 32 bits of it live, so the planes are taken 8 at a time and the
 * work follows ceil(nout/8) groups rather than a full 32 rows.  LADDER421Q
 * on a group of 8 planes leaves, in row i lane w byte t, the value byte
 * (bits 8g..8g+7) of hand 64w+8t+i; bytetr8 turns that into 16 consecutive
 * hands' bytes per lane per register; a vpunpck bw/wd tree merges the
 * <= 4 groups' bytes into u32s; one vperm2i128 per output register lands
 * each lane's 8 hands contiguously.
 */

#include <immintrin.h>

#define LD256(p)     _mm256_loadu_si256((const __m256i *)(p))
#define ST256(p, v)  _mm256_storeu_si256((__m256i *)(p), (v))
#define LD128(p)     _mm_loadu_si128((const __m128i *)(p))

/* one swap: exchange the M-masked bits of lo shifted down by J with the
 * corresponding bits of hi (transpose64_portable.h's SW in intrinsics) */
#define SWQ(J, M, lo, hi)                                                 \
    do {                                                                  \
        __m256i t_ = _mm256_and_si256(                                    \
            _mm256_xor_si256(_mm256_srli_epi64(lo, J), hi), M);           \
        hi = _mm256_xor_si256(hi, t_);                                    \
        lo = _mm256_xor_si256(lo, _mm256_slli_epi64(t_, J));              \
    } while (0)

/* stages 4,2,1 over eight rows in registers */
#define LADDER421Q(r)                                                     \
    do {                                                                  \
        const __m256i m4_ = _mm256_set1_epi8(0x0F);                       \
        const __m256i m2_ = _mm256_set1_epi8(0x33);                       \
        const __m256i m1_ = _mm256_set1_epi8(0x55);                       \
        SWQ(4, m4_, r[0], r[4]); SWQ(4, m4_, r[1], r[5]);                 \
        SWQ(4, m4_, r[2], r[6]); SWQ(4, m4_, r[3], r[7]);                 \
        SWQ(2, m2_, r[0], r[2]); SWQ(2, m2_, r[1], r[3]);                 \
        SWQ(2, m2_, r[4], r[6]); SWQ(2, m2_, r[5], r[7]);                 \
        SWQ(1, m1_, r[0], r[1]); SWQ(1, m1_, r[2], r[3]);                 \
        SWQ(1, m1_, r[4], r[5]); SWQ(1, m1_, r[6], r[7]);                 \
    } while (0)

/* 8x8 byte transpose per u64 lane across rows r[0..7]: with T_t the
 * qword whose byte i is r[i]'s byte t (per lane),
 *   out[2p]   = [lane 0: T_2p T_2p+1 | lane 2: T_2p T_2p+1]
 *   out[2p+1] = [lane 1: T_2p T_2p+1 | lane 3: T_2p T_2p+1]
 * (vpunpck works per 128-bit half, so the tree keeps lanes 0/2 in one
 * register and 1/3 in the other). */
static inline __attribute__((always_inline)) void
bytetr8(const __m256i r[8], __m256i out[8])
{
    __m256i a0 = _mm256_unpacklo_epi8(r[0], r[1]), b0 = _mm256_unpackhi_epi8(r[0], r[1]);
    __m256i a1 = _mm256_unpacklo_epi8(r[2], r[3]), b1 = _mm256_unpackhi_epi8(r[2], r[3]);
    __m256i a2 = _mm256_unpacklo_epi8(r[4], r[5]), b2 = _mm256_unpackhi_epi8(r[4], r[5]);
    __m256i a3 = _mm256_unpacklo_epi8(r[6], r[7]), b3 = _mm256_unpackhi_epi8(r[6], r[7]);
    __m256i c0 = _mm256_unpacklo_epi16(a0, a1), c1 = _mm256_unpackhi_epi16(a0, a1);
    __m256i c2 = _mm256_unpacklo_epi16(a2, a3), c3 = _mm256_unpackhi_epi16(a2, a3);
    __m256i d0 = _mm256_unpacklo_epi16(b0, b1), d1 = _mm256_unpackhi_epi16(b0, b1);
    __m256i d2 = _mm256_unpacklo_epi16(b2, b3), d3 = _mm256_unpackhi_epi16(b2, b3);
    out[0] = _mm256_unpacklo_epi32(c0, c2);   /* T_0 T_1, lanes 0|2 */
    out[2] = _mm256_unpackhi_epi32(c0, c2);   /* T_2 T_3 */
    out[4] = _mm256_unpacklo_epi32(c1, c3);   /* T_4 T_5 */
    out[6] = _mm256_unpackhi_epi32(c1, c3);   /* T_6 T_7 */
    out[1] = _mm256_unpacklo_epi32(d0, d2);   /* the same, lanes 1|3 */
    out[3] = _mm256_unpackhi_epi32(d0, d2);
    out[5] = _mm256_unpacklo_epi32(d1, d3);
    out[7] = _mm256_unpackhi_epi32(d1, d3);
}

/* ---- pack ---------------------------------------------------------------- */

/* batch-layout rows i, i+1 (lane w of row i = m[64w+i]) into registers:
 * four broadcast loads pair hands (i, i+1) of lanes 0 and 2 / 1 and 3
 * into one register each (vpblendd keeps the half it wants), and one
 * vpunpck{l,h}qdq each finishes the 2x2 */
static inline __attribute__((always_inline)) void
gather_pair(const uint64_t *m, int i, __m256i *r0, __m256i *r1)
{
    __m256i a = _mm256_blend_epi32(_mm256_broadcastsi128_si256(LD128(m + i)),
                                   _mm256_broadcastsi128_si256(LD128(m + 128 + i)), 0xF0);
    __m256i b = _mm256_blend_epi32(_mm256_broadcastsi128_si256(LD128(m + 64 + i)),
                                   _mm256_broadcastsi128_si256(LD128(m + 192 + i)), 0xF0);
    *r0 = _mm256_unpacklo_epi64(a, b);
    *r1 = _mm256_unpackhi_epi64(a, b);
}

/* pass 2 for one stride-8 row set: rows k0+8i, i < 8, through stages
 * 8/16/32 (= bytetr8 + re-pairing the lanes), then to the planes.
 * pack_fused calls it from a rolled loop over k0, so the map variant's
 * idx < 52 test runs per plane rather than folding: one copy of this
 * pass instead of eight, which would be most of the pack's text. */
static inline __attribute__((always_inline)) void
pack_pass2(const __m256i rows[64], int k0, const int *map, bs_word *planes,
           int use_map)
{
    __m256i r[8], o[8];
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) r[i] = rows[k0 + 8 * i];
    bytetr8(r, o);
#pragma GCC unroll 4
    for (int p = 0; p < 4; p++) {
        r[2 * p]     = _mm256_unpacklo_epi64(o[2 * p], o[2 * p + 1]);
        r[2 * p + 1] = _mm256_unpackhi_epi64(o[2 * p], o[2 * p + 1]);
    }
    if (use_map) {
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++) {
            int idx = k0 + 8 * i;
            if (idx < 52) planes[map[idx]] = (bs_word)r[i];
        }
    } else {
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++) planes[k0 + 8 * i] = (bs_word)r[i];
    }
}

/* map = NULL: write all 64 planes in place (bs_transpose's contract);
 * else write plane c to planes[map[c]], c < 52 only.  always_inline so
 * the NULL test folds per call site. */
static inline __attribute__((always_inline)) void
pack_fused(const uint64_t masks[BS_BATCH], const int *map, bs_word *planes,
           int use_map)
{
    __m256i rows[64];
    for (int g = 0; g < 64; g += 8) {
        __m256i r[8];
        gather_pair(masks, g,     &r[0], &r[1]);
        gather_pair(masks, g + 2, &r[2], &r[3]);
        gather_pair(masks, g + 4, &r[4], &r[5]);
        gather_pair(masks, g + 6, &r[6], &r[7]);
        LADDER421Q(r);
#pragma GCC unroll 8
        for (int i = 0; i < 8; i++) rows[g + i] = r[i];
    }
#pragma GCC unroll 1
    for (int k0 = 0; k0 < 8; k0++)
        pack_pass2(rows, k0, map, planes, use_map);
}

void bs_transpose(const uint64_t masks[BS_BATCH], bs_word planes[64])
{
    pack_fused(masks, NULL, planes, 0);
}

void bs_transpose_map(const uint64_t masks[BS_BATCH], const int map[52],
                      bs_word *planes)
{
    pack_fused(masks, map, planes, 1);
}

/* ---- unpack -------------------------------------------------------------- */

/* ng = number of 8-plane groups (1..4), a literal at each call site.  The
 * group loop below is left rolled for its size, so the zero groups are
 * skipped at run time rather than folded out.  Group g's eight bytetr8
 * outputs U[g][s], s = 2q+h, hold value byte g of hands 64h+16q+0..15 in
 * the low half and of hands 64(h+2)+16q+0..15 in the high half. */
static inline __attribute__((always_inline)) void
unpack_core(const bs_word *planes, int nout, uint32_t vals[BS_BATCH], int ng)
{
    __m256i U[4][8];
    /* Pinned rolled: unrolling copies the group body four times, and this
     * code shares an instruction cache with the circuit that runs between
     * the pack and the unpack, where the text costs more than the loop
     * overhead it saves.  Nothing inside folds any more -- g against ng and
     * the plane index against nout are both tested per group, and U is
     * addressed by a variable index, so it lives on the stack rather than
     * in registers.  The pragma is what pins it: gcc unrolls the loop on
     * its own at -O3. */
#pragma GCC unroll 1
    for (int g = 0; g < 4; g++) {
        if (g < ng) {
            __m256i r[8];
#pragma GCC unroll 8
            for (int l = 0; l < 8; l++)
                r[l] = 8 * g + l < nout ? (__m256i)planes[8 * g + l]
                                        : _mm256_setzero_si256();
            LADDER421Q(r);
            bytetr8(r, U[g]);
        } else {
#pragma GCC unroll 8
            for (int s = 0; s < 8; s++) U[g][s] = _mm256_setzero_si256();
        }
    }
    /* rolled on the same terms: one store body instead of eight */
#pragma GCC unroll 1
    for (int s = 0; s < 8; s++) {
        int q = s >> 1, h = s & 1;
        /* bytes -> u16 (b0,b1) and (b2,b3) -> u32, hands 0-7 / 8-15 of 16 */
        __m256i m1 = _mm256_unpacklo_epi8(U[0][s], U[1][s]);
        __m256i m2 = _mm256_unpackhi_epi8(U[0][s], U[1][s]);
        __m256i z1 = _mm256_unpacklo_epi8(U[2][s], U[3][s]);
        __m256i z2 = _mm256_unpackhi_epi8(U[2][s], U[3][s]);
        __m256i da = _mm256_unpacklo_epi16(m1, z1);   /* hands 16q+0..3  [lane h | lane h+2] */
        __m256i db = _mm256_unpackhi_epi16(m1, z1);   /* 16q+4..7 */
        __m256i dc = _mm256_unpacklo_epi16(m2, z2);   /* 16q+8..11 */
        __m256i dd = _mm256_unpackhi_epi16(m2, z2);   /* 16q+12..15 */
        uint32_t *pa = vals + 64 * h + 16 * q, *pb = vals + 64 * (h + 2) + 16 * q;
        ST256(pa,     _mm256_permute2x128_si256(da, db, 0x20));
        ST256(pa + 8, _mm256_permute2x128_si256(dc, dd, 0x20));
        ST256(pb,     _mm256_permute2x128_si256(da, db, 0x31));
        ST256(pb + 8, _mm256_permute2x128_si256(dc, dd, 0x31));
    }
}

void bs_untranspose(const bs_word *planes, int nout, uint32_t vals[BS_BATCH])
{
    switch ((nout + 7) >> 3) {
    case 0:
    case 1:  unpack_core(planes, nout, vals, 1); return;
    case 2:  unpack_core(planes, nout, vals, 2); return;
    case 3:  unpack_core(planes, nout, vals, 3); return;
    default: unpack_core(planes, nout, vals, 4); return;
    }
}

#undef LD256
#undef ST256
#undef LD128
