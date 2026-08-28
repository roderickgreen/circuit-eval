#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_masks_from_cards, AVX-512 VBMI core.
 *
 * The core takes ncards as a parameter and is always_inline; the switch at
 * the bottom pins it to a literal per call, as the scalar and AVX2 cores do,
 * so the card loop unrolls to a fixed length. */

/* Eight hands per step.  Hands 0..3 of the group sit inside one 64-byte
 * load and hands 4..7 inside another (any stride up to 16 fits), so one
 * two-source byte permute (vpermi2b) gathers card j of all eight hands
 * into the low byte of its qword lane -- the write mask zeroes the other
 * seven bytes, which zero-extends the ids for free.  A variable qword
 * shift turns ids into single-bit masks and an OR accumulates; the index
 * vector for card j+1 is the card-j vector plus one. */

#include <immintrin.h>

static inline __attribute__((always_inline)) void
avx512_masks_fixed(const uint8_t *cards, size_t stride,
                   uint64_t masks[BS_BATCH], int ncards)
{
    const __mmask64 lanes = 0x0101010101010101ull;
    const __m512i one_q = _mm512_set1_epi64(1);
    const __m512i one_b = _mm512_set1_epi8(1);
    uint8_t idx0[64] = {0};
    for (int h = 0; h < 4; h++) {
        idx0[8 * h] = (uint8_t)(h * stride);
        idx0[8 * (h + 4)] = (uint8_t)(64 + h * stride);
    }
    const __m512i base = _mm512_loadu_si512(idx0);
    /* a group's furthest load ends 4*stride + 64 bytes past its base; the
     * last group or two would read past the array (how many depends on the
     * stride), so those copy their own 8 hands into a zero-padded buffer */
    const size_t total = (size_t)(BS_BATCH - 1) * stride + (size_t)ncards;
    for (int g = 0; g < BS_BATCH / 8; g++) {
        uint8_t buf[128];
        const uint8_t *p = cards + (size_t)g * 8 * stride;
        if ((size_t)g * 8 * stride + 4 * stride + 64 > total) {
            memset(buf, 0, sizeof buf);
            memcpy(buf, p, 7 * stride + (size_t)ncards);
            p = buf;
        }
        __m512i a = _mm512_loadu_si512(p);
        __m512i b = _mm512_loadu_si512(p + 4 * stride);
        __m512i idx = base;
        __m512i m = _mm512_setzero_si512();
#pragma GCC unroll 8
        for (int j = 0; j < ncards; j++) {
            __m512i c = _mm512_maskz_permutex2var_epi8(lanes, a, idx, b);
            m = _mm512_or_si512(m, _mm512_sllv_epi64(one_q, c));
            idx = _mm512_add_epi8(idx, one_b);
        }
        _mm512_storeu_si512(masks + 8 * g, m);
    }
}

void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH])
{
    if (ncards < 1 || ncards > 8 || stride > 16) {
        masks_from_cards_scalar(cards, ncards, stride, masks);
        return;
    }
    switch (ncards) {
    case 4:  avx512_masks_fixed(cards, stride, masks, 4); return;
    case 5:  avx512_masks_fixed(cards, stride, masks, 5); return;
    case 6:  avx512_masks_fixed(cards, stride, masks, 6); return;
    case 7:  avx512_masks_fixed(cards, stride, masks, 7); return;
    default: avx512_masks_fixed(cards, stride, masks, ncards); return;
    }
}
