#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_masks_from_cards, AVX2 core. */

/* Four hands per step: the AVX-512 VBMI scheme at 256-bit scale.  Each
 * hand's cards are ncards <= 8 contiguous bytes, so the group loads as
 * four 8-byte hand qwords assembled into ymm lanes by broadcast+blend
 * (vpblendd runs on any ALU port; a vpinsrq chain would serialize on the
 * shuffle port).  Per card, one vpshufb gathers card j of all four hands
 * into the low byte of its qword lane -- the control vector holds 0x80 in
 * the other seven byte positions, which zeroes them and so zero-extends
 * the ids for free (and stays out of range under the +1-per-card
 * increment: 0x80 only grows) -- then vpsllvq turns ids into single-bit
 * masks and an OR accumulates.  Four single-uop ops per card per four
 * hands, wrapped in the same switch-pinned-ncards dispatch as the scalar
 * core. */

#include <immintrin.h>

static inline uint64_t load_u64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* 4 hands into qword lanes */
static inline __m256i grp4(const uint8_t *p, size_t stride)
{
    __m256i b0 = _mm256_set1_epi64x((long long)load_u64(p));
    __m256i b1 = _mm256_set1_epi64x((long long)load_u64(p + stride));
    __m256i b2 = _mm256_set1_epi64x((long long)load_u64(p + 2 * stride));
    __m256i b3 = _mm256_set1_epi64x((long long)load_u64(p + 3 * stride));
    __m256i t01 = _mm256_blend_epi32(b0, b1, 0x0C);
    __m256i t23 = _mm256_blend_epi32(b2, b3, 0xC0);
    return _mm256_blend_epi32(t01, t23, 0xF0);
}

static inline __attribute__((always_inline)) void
avx2_masks_fixed(const uint8_t *cards, size_t stride,
                 uint64_t masks[BS_BATCH], int ncards)
{
    /* control for card 0: low byte of each qword reads the hand's card 0
     * (in-lane offset 0 or 8; vpshufb is per 128 bits) */
    const __m256i ctl0 = _mm256_setr_epi8(
        0, -128, -128, -128, -128, -128, -128, -128,
        8, -128, -128, -128, -128, -128, -128, -128,
        0, -128, -128, -128, -128, -128, -128, -128,
        8, -128, -128, -128, -128, -128, -128, -128);
    const __m256i one_b = _mm256_set1_epi8(1);
    const __m256i one_q = _mm256_set1_epi64x(1);

    /* a group's per-hand qword loads read up to 3*stride + 8 bytes past
     * the group base regardless of how few of them ncards needs, so the
     * last group or two -- whose window runs past the array -- copy their
     * hands into a zero-padded buffer (zeros are never selected: the
     * control only reads bytes j < ncards), keeping the header's
     * no-overread promise */
    const size_t total = (size_t)(BS_BATCH - 1) * stride + (size_t)ncards;
    for (int g = 0; g < BS_BATCH / 4; g++) {
        const uint8_t *p = cards + (size_t)g * 4 * stride;
        __m256i h;
        if ((size_t)g * 4 * stride + 3 * stride + 8 > total) {
            uint8_t buf[32] = {0};
            for (int k = 0; k < 4; k++)
                memcpy(buf + 8 * k, p + (size_t)k * stride, (size_t)ncards);
            h = _mm256_loadu_si256((const __m256i *)buf);
        } else {
            h = grp4(p, stride);
        }
        __m256i ctl = ctl0;
        __m256i m = _mm256_setzero_si256();
#pragma GCC unroll 8
        for (int j = 0; j < ncards; j++) {
            __m256i c = _mm256_shuffle_epi8(h, ctl);
            m = _mm256_or_si256(m, _mm256_sllv_epi64(one_q, c));
            ctl = _mm256_add_epi8(ctl, one_b);
        }
        _mm256_storeu_si256((__m256i *)(masks + 4 * g), m);
    }
}

void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH])
{
    if (ncards < 1 || ncards > 8) {
        masks_from_cards_scalar(cards, ncards, stride, masks);
        return;
    }
    switch (ncards) {
    case 4:  avx2_masks_fixed(cards, stride, masks, 4); return;
    case 5:  avx2_masks_fixed(cards, stride, masks, 5); return;
    case 6:  avx2_masks_fixed(cards, stride, masks, 6); return;
    case 7:  avx2_masks_fixed(cards, stride, masks, 7); return;
    default: avx2_masks_fixed(cards, stride, masks, ncards); return;
    }
}
