#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_masks_from_cards, scalar core.  Always compiled: the vector cores
 * fall back to it for shapes they refuse, and it is the public entry
 * point when BS_MASKS_SCALAR is set (MASKS=scalar, or no vector core for
 * the target -- see bspack.c). */

static inline __attribute__((always_inline)) void
masks_fixed(const uint8_t *cards, size_t stride, uint64_t masks[BS_BATCH],
            int ncards)
{
    for (int l = 0; l < BS_BATCH; l++) {
        const uint8_t *c = cards + (size_t)l * stride;
        uint64_t m = 0;
#pragma GCC unroll 8
        for (int j = 0; j < ncards; j++)
            m |= 1ull << c[j];
        masks[l] = m;
    }
}

/* Under a vector core this is only the fallback for shapes that core
 * refuses, reached on no call the API's own users make, and a compiler
 * left to itself inlines all five of its instantiations into the middle
 * of the vector entry point -- a third to twice that function's size, in
 * code no batch executes.  noinline+cold moves it to its own symbol in
 * the cold text section.  Where it is the entry point itself
 * (BS_MASKS_SCALAR) it is the working core and gets neither. */
#ifdef BS_MASKS_SCALAR
#define BS_MASKS_FALLBACK_ATTR
#else
#define BS_MASKS_FALLBACK_ATTR __attribute__((noinline, cold))
#endif

/* The switch pins the inner trip count per call site so the unrolled
 * card loop has no tail iterations to guard. */
static BS_MASKS_FALLBACK_ATTR void
masks_from_cards_scalar(const uint8_t *cards, int ncards,
                        size_t stride, uint64_t masks[BS_BATCH])
{
    switch (ncards) {
    case 4:  masks_fixed(cards, stride, masks, 4); return;
    case 5:  masks_fixed(cards, stride, masks, 5); return;
    case 6:  masks_fixed(cards, stride, masks, 6); return;
    case 7:  masks_fixed(cards, stride, masks, 7); return;
    default: masks_fixed(cards, stride, masks, ncards); return;
    }
}

#ifdef BS_MASKS_SCALAR

void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH])
{
    masks_from_cards_scalar(cards, ncards, stride, masks);
}

#endif /* BS_MASKS_SCALAR */
