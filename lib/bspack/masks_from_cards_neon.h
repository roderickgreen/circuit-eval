#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_masks_from_cards, arm64/NEON core. */

/* Two hands per step.  Both hands of a pair sit inside one 32-byte window
 * (stride + ncards <= 32, checked below), so a single vqtbl2q gathers card
 * j of both hands at once -- the index vector puts hand A's byte at index 0
 * and hand B's at index 8, everything else at an out-of-range sentinel
 * (0xFF), so vqtbl2q's table zero-extends both ids straight into the low
 * byte of their own u64 lane: reinterpreting the result as uint64x2_t needs
 * no further widening.  vshlq_u64 turns ids into single-bit masks and
 * vorrq_u64 accumulates; the index vector for card j+1 is card j's plus
 * one, added only in the two active byte lanes so the sentinel bytes never
 * drift into range.
 *
 * The switch pins ncards to a literal per call site (as in the scalar
 * core), so the card loop unrolls with no tail guard. */

#include <arm_neon.h>

static inline __attribute__((always_inline)) void
neon_masks_fixed(const uint8_t *cards, size_t stride, uint64_t masks[BS_BATCH],
                 int ncards)
{
    uint8_t idx0_bytes[16];
    memset(idx0_bytes, 0xFF, sizeof idx0_bytes);
    idx0_bytes[0] = 0;
    idx0_bytes[8] = (uint8_t)stride;
    const uint8x16_t idx0 = vld1q_u8(idx0_bytes);

    uint8_t inc_bytes[16] = {0};
    inc_bytes[0] = 1;
    inc_bytes[8] = 1;
    const uint8x16_t inc = vld1q_u8(inc_bytes);
    const uint64x2_t one = vdupq_n_u64(1);

    /* the two vld1q_u8 below unconditionally read a full 32-byte window
     * from tp regardless of how much of it stride+ncards actually needs,
     * so the last pair or two -- whose window runs past the array -- must
     * be caught on that 32-byte extent, not the smaller needed one; those
     * copy their own two hands into a zero-padded buffer, same contract as
     * the AVX-512 core */
    const size_t total = (size_t)(BS_BATCH - 1) * stride + (size_t)ncards;
    for (int p = 0; p < BS_BATCH; p += 2) {
        uint8_t buf[32];
        const uint8_t *tp = cards + (size_t)p * stride;
        if ((size_t)p * stride + 32 > total) {
            memset(buf, 0, sizeof buf);
            memcpy(buf, tp, total - (size_t)p * stride);
            tp = buf;
        }
        const uint8x16x2_t tbl = {vld1q_u8(tp), vld1q_u8(tp + 16)};
        uint8x16_t idx = idx0;
        uint64x2_t acc = vdupq_n_u64(0);
#pragma GCC unroll 8
        for (int j = 0; j < ncards; j++) {
            uint64x2_t val = vreinterpretq_u64_u8(vqtbl2q_u8(tbl, idx));
            acc = vorrq_u64(acc, vshlq_u64(one, vreinterpretq_s64_u64(val)));
            idx = vaddq_u8(idx, inc);
        }
        vst1q_u64(&masks[p], acc);
    }
}

void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH])
{
    if (ncards < 1 || ncards > 8 || stride + (size_t)ncards > 32) {
        masks_from_cards_scalar(cards, ncards, stride, masks);
        return;
    }
    switch (ncards) {
    case 4:  neon_masks_fixed(cards, stride, masks, 4); return;
    case 5:  neon_masks_fixed(cards, stride, masks, 5); return;
    case 6:  neon_masks_fixed(cards, stride, masks, 6); return;
    case 7:  neon_masks_fixed(cards, stride, masks, 7); return;
    default: neon_masks_fixed(cards, stride, masks, ncards); return;
    }
}
