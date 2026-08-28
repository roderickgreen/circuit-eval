#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_transpose / bs_transpose_map / bs_untranspose, arm64/NEON core.
 * Follows transpose64_neon.h, whose TRN*, SWBSL/LADDER421 and
 * DECL_MASKS macros it shares.  Fused schedules in the manner of pack_avx2.h, in place of the
 * gather + bs_transpose_64x64 + scatter composition bspack.c builds for
 * targets without one.
 *
 * Lane geometry as in transpose64_neon.h: the BS_LANES u64 lanes of a
 * bs_word are processed as BS_LANES/2 independent uint64x2_t halves
 * (half h = hands 128h..128h+127, lanes 2h and 2h+1), one after the
 * other, so every schedule below is written for 128 hands and the h loop
 * repeats it.
 *
 * Pack is the 64x64 ladder's two passes in the other order, with the
 * batch-layout gather fused into the first and the plane stores into the
 * second:
 *
 *  pass 1, per stride-8 row set: row k0+8i is one lane load per lane --
 *     hand k0+8i of lane 2h into the low u64, hand 64+k0+8i of lane 2h+1
 *     into the high (two 8-byte loads and an ins: the same one vector op
 *     per row as a trn1/trn2 gather of row pairs, no separate gather
 *     pass) --
 *     then stages 32,16,8 (the trn pairs), then the set is stored.
 *  pass 2, per contiguous 8-row block: stages 4,2,1, stores to planes[c]
 *     or, in the map variant, to planes[map[c]] for c < 52 and nowhere
 *     for c >= 52.  The block loop is unrolled with g a literal so those
 *     tests fold: block 56 disappears whole, loads included, and the half
 *     of block 48 feeding planes 52..55 with it -- 60 of the pass's 320
 *     vector ops per half.  The pass order is load-bearing for that
 *     fold: with the bit stages first and the trn pass last, the trns
 *     feeding a dead row mostly also feed a live one and only 28 ops
 *     fold (measured 6% slower with map; the no-map shape is
 *     indifferent to the order).
 *
 * Unpack never forms the 64x64 matrix: only nout planes carry anything,
 * and one 8-plane group at a time is enough.  Per group: stages 4,2,1
 * over its eight rows turn row l, byte b into value byte g of hand 8b+l;
 * the 8x8 byte transpose (the same three trn rounds as pass 1 above)
 * turns that into one register per b holding value byte g of hands
 * 8b..8b+7, lane 2h in the low half and lane 2h+1 in the high; then a
 * u8 zip across groups 0/1 and 2/3 and a u16 zip across those makes the
 * u32 values, eight contiguous hands per lane per b, two 16-byte stores
 * each.  nout rounds up to 8-plane groups, pinned per call site so the
 * zero groups fold: 8 planes cost one group, 24 cost three.
 */

/* ---- pack ---------------------------------------------------------------- */

/* use_map 0: write all 64 planes in place (bs_transpose's contract).
 * use_map 1: write plane c to planes[map[c]], c < 52 only.  always_inline,
 * use_map a literal at both call sites, so the unused arm folds. */
static inline __attribute__((always_inline)) void
pack_fused(const uint64_t masks[BS_BATCH], const int *map, bs_word *planes,
           int use_map)
{
    DECL_MASKS;
    bs_word scratch[64];
    bs_word *rows = use_map ? scratch : planes;

    for (int h = 0; h < BS_LANES / 2; h++) {
        const uint64_t *mm = masks + 128 * h;   /* lanes 2h, 2h+1 */
        uint64_t *p = (uint64_t *)rows + 2 * h;

        /* pass 1: lane-load gather + stages 32,16,8 over stride-8 row sets */
        for (int k0 = 0; k0 < 8; k0++) {
            uint64x2_t r[8];
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                r[i] = vcombine_u64(vld1_u64(mm + k0 + 8 * i),
                                    vld1_u64(mm + 64 + k0 + 8 * i));
            TRN32(r[0], r[4]); TRN32(r[1], r[5]);
            TRN32(r[2], r[6]); TRN32(r[3], r[7]);
            TRN16(r[0], r[2]); TRN16(r[1], r[3]);
            TRN16(r[4], r[6]); TRN16(r[5], r[7]);
            TRN8(r[0], r[1]); TRN8(r[2], r[3]);
            TRN8(r[4], r[5]); TRN8(r[6], r[7]);
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                vst1q_u64(p + BS_LANES * (k0 + 8 * i), r[i]);
        }

        /* pass 2: stages 4,2,1 over contiguous 8-row blocks, plane stores */
#pragma GCC unroll 8
        for (int g = 0; g < 64; g += 8) {
            if (use_map && g >= 52) continue;
            uint64x2_t r[8];
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++)
                r[i] = vld1q_u64(p + BS_LANES * (g + i));
            LADDER421(r);
#pragma GCC unroll 8
            for (int i = 0; i < 8; i++) {
                const int c = g + i;
                if (!use_map)
                    vst1q_u64(p + BS_LANES * c, r[i]);
                else if (c < 52)
                    vst1q_u64((uint64_t *)&planes[map[c]] + 2 * h, r[i]);
            }
        }
    }
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

/* ng = number of 8-plane groups (1..4), a literal at each call site so the
 * zero groups fold away.  V[g][b] = value byte g of hands 8b..8b+7, lane
 * 2h in bytes 0..7 and lane 2h+1 in bytes 8..15. */
static inline __attribute__((always_inline)) void
unpack_core(const bs_word *planes, int nout, uint32_t vals[BS_BATCH], int ng)
{
    DECL_MASKS;
    const uint64x2_t zero = vdupq_n_u64(0);

    for (int h = 0; h < BS_LANES / 2; h++) {
        const uint64_t *p = (const uint64_t *)planes + 2 * h;
        uint64x2_t V[4][8];
#pragma GCC unroll 4
        for (int g = 0; g < 4; g++) {
            if (g < ng) {
                uint64x2_t r[8];
#pragma GCC unroll 8
                for (int l = 0; l < 8; l++)
                    r[l] = 8 * g + l < nout
                               ? vld1q_u64(p + BS_LANES * (8 * g + l))
                               : zero;
                LADDER421(r);   /* row l byte b = byte g of hand 8b+l */
                TRN32(r[0], r[4]); TRN32(r[1], r[5]);   /* 8x8 byte transpose */
                TRN32(r[2], r[6]); TRN32(r[3], r[7]);   /* per lane: row b   */
                TRN16(r[0], r[2]); TRN16(r[1], r[3]);   /* byte l = byte g   */
                TRN16(r[4], r[6]); TRN16(r[5], r[7]);   /* of hand 8b+l      */
                TRN8(r[0], r[1]); TRN8(r[2], r[3]);
                TRN8(r[4], r[5]); TRN8(r[6], r[7]);
#pragma GCC unroll 8
                for (int b = 0; b < 8; b++) V[g][b] = r[b];
            } else {
#pragma GCC unroll 8
                for (int b = 0; b < 8; b++) V[g][b] = zero;
            }
        }

        /* bytes -> u16 (b0,b1) and (b2,b3) -> u32, hands 8b+0..3 / 4..7 */
        uint32_t *va = vals + 64 * (2 * h), *vb = vals + 64 * (2 * h + 1);
#pragma GCC unroll 8
        for (int b = 0; b < 8; b++) {
            uint8x16_t v0 = vreinterpretq_u8_u64(V[0][b]);
            uint8x16_t v1 = vreinterpretq_u8_u64(V[1][b]);
            uint8x16_t v2 = vreinterpretq_u8_u64(V[2][b]);
            uint8x16_t v3 = vreinterpretq_u8_u64(V[3][b]);
            uint16x8_t a01 = vreinterpretq_u16_u8(vzip1q_u8(v0, v1));  /* lane 2h   */
            uint16x8_t a23 = vreinterpretq_u16_u8(vzip1q_u8(v2, v3));
            uint16x8_t b01 = vreinterpretq_u16_u8(vzip2q_u8(v0, v1));  /* lane 2h+1 */
            uint16x8_t b23 = vreinterpretq_u16_u8(vzip2q_u8(v2, v3));
            vst1q_u32(va + 8 * b,     vreinterpretq_u32_u16(vzip1q_u16(a01, a23)));
            vst1q_u32(va + 8 * b + 4, vreinterpretq_u32_u16(vzip2q_u16(a01, a23)));
            vst1q_u32(vb + 8 * b,     vreinterpretq_u32_u16(vzip1q_u16(b01, b23)));
            vst1q_u32(vb + 8 * b + 4, vreinterpretq_u32_u16(vzip2q_u16(b01, b23)));
        }
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
