#ifndef BSPACK_INTERNAL
#error "this file is a section of bspack.c and is compiled only through it"
#endif
/* bs_masks_from_cards, portable vector core.  Selected only by
 * BS_PORTABLE_MASKS (lib/Makefile MASKS=portable) --
 * see the requirement below for why it is not a default anywhere.
 *
 * BS_LANES hands per step, one hand's card bytes per u64 lane, which is
 * the scheme the instruction-set cores use with their lane-crossing pieces
 * replaced by operators:
 *
 *   group load     lane k takes hand k's eight bytes.  Each hand's u64 is
 *                  broadcast to every lane, which from memory is a plain
 *                  load, and the broadcasts are merged: at four lanes by
 *                  lane selects (a tree of two-operand shuffles with
 *                  constant indices, which compile to blends), elsewhere
 *                  by masking each down to its lane and OR'ing.  One mask
 *                  then clears the bytes at or above ncards in every lane,
 *                  which leaves byte 7 of every lane zero.  BS_PIN on each
 *                  broadcast keeps the compiler from re-lowering the
 *                  merge through scalar registers.
 *   card extract   card j of every hand into the low byte of its lane is a
 *                  byte permute with constant indices: byte 8k+j to
 *                  position 8k, and the lane's other seven positions read
 *                  byte 8k+7, the one the group load zeroed.  That
 *                  zero-extends the id in the same operation, so the
 *                  extract is one op and needs no control register.
 *   id -> bit      one << id, an element-wise variable shift.
 *
 * The extract falls back to a shift and a mask, (h >> 8j) & 63, in the two
 * cases the permute cannot serve: ncards == 8, which fills every byte of a
 * lane and so leaves no zeroed byte to read, and a bs_word wider than the
 * target's widest vector type.  Compilers split every other operation here
 * across the pieces of an emulated word, but not a constant byte permute:
 * gcc lowers that one a byte at a time through memory, which costs more
 * than the whole rest of the kernel.  BS_BATCH pinned above the target's
 * width is a supported configuration (bsapi.h), so the width test below
 * decides which form is compiled.  The mask is 63 rather than 255 because
 * card ids are below 64 and a shift count at or above the element width is
 * undefined for GNU vector shifts.
 *
 * Requirement: the target's vector unit must have a per-element variable
 * shift (x86 vpsllvq, arm64 ushl, and the equivalent elsewhere).  Where it
 * does not, compilers lower one << id by extracting each lane to a scalar
 * register and reinserting it, which is slower than
 * masks_from_cards_scalar -- so a target without that instruction wants
 * the scalar core, and this one is worth measuring against it before being
 * turned on.
 */

static inline uint64_t load_u64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* the low n bytes set, n <= 8 */
static const uint64_t low_bytes[9] = {
    0, 0xffull, 0xffffull, 0xffffffull, 0xffffffffull, 0xffffffffffull,
    0xffffffffffffull, 0xffffffffffffffull, ~0ull
};

typedef unsigned char bs_bytes __attribute__((vector_size(BS_BATCH / 8)));

/* BS_WORD_IN_REG (bspack.c) is the test for "bs_word is not emulated" --
 * see the header comment for what the permute costs where it is. */
#if BS_WORD_IN_REG
#define BS_PERMUTE_EXTRACT 1
#else
#define BS_PERMUTE_EXTRACT 0
#endif

/* Per lane count: the lane index vector the group load's lane masks are
 * built from, and card_byte(h, j) -- byte 8k+j of h to position 8k, the
 * rest of lane k reading byte 8k+7.  These are the only two places the
 * lane count is spelled out. */
#define FILL7(k) 8*(k)+7, 8*(k)+7, 8*(k)+7, 8*(k)+7, 8*(k)+7, 8*(k)+7, 8*(k)+7
#if BS_LANES == 8
#define LANE_IDS {0, 1, 2, 3, 4, 5, 6, 7}
#define card_byte(h, j)                                                   \
    ((bs_word)__builtin_shufflevector((bs_bytes)(h), (bs_bytes)(h),       \
        (j), FILL7(0),  8 + (j), FILL7(1), 16 + (j), FILL7(2),            \
        24 + (j), FILL7(3), 32 + (j), FILL7(4), 40 + (j), FILL7(5),       \
        48 + (j), FILL7(6), 56 + (j), FILL7(7)))
#elif BS_LANES == 4
#define LANE_IDS {0, 1, 2, 3}
#define card_byte(h, j)                                                   \
    ((bs_word)__builtin_shufflevector((bs_bytes)(h), (bs_bytes)(h),       \
        (j), FILL7(0),  8 + (j), FILL7(1), 16 + (j), FILL7(2),            \
        24 + (j), FILL7(3)))
#elif BS_LANES == 2
#define LANE_IDS {0, 1}
#define card_byte(h, j)                                                   \
    ((bs_word)__builtin_shufflevector((bs_bytes)(h), (bs_bytes)(h),       \
        (j), FILL7(0),  8 + (j), FILL7(1)))
#else
#define LANE_IDS {0}
#define card_byte(h, j)                                                   \
    ((bs_word)__builtin_shufflevector((bs_bytes)(h), (bs_bytes)(h),       \
        (j), FILL7(0)))
#endif

/* hand k of the group into lane k, bytes at or above ncards cleared.
 * Vectors stay out of these signatures: a bs_word wider than the target's
 * native vector width is an ABI change when passed by value, which the
 * compiler is right to warn about. */
static inline __attribute__((always_inline)) void
group_lanes(const uint8_t *p, size_t stride, int ncards, bs_word *out)
{
    const bs_word zero = {0};
    const bs_word keep = zero + low_bytes[ncards];
    bs_word h = zero;
#if BS_LANES == 4
    {   bs_word b0 = zero + load_u64(p), b1 = zero + load_u64(p + stride),
                b2 = zero + load_u64(p + 2 * stride), b3 = zero + load_u64(p + 3 * stride);
        BS_PIN(b0); BS_PIN(b1); BS_PIN(b2); BS_PIN(b3);
        h = __builtin_shufflevector(__builtin_shufflevector(b0, b1, 0, 5, 2, 3),
                                    __builtin_shufflevector(b2, b3, 0, 1, 2, 7),
                                    0, 1, 6, 7);
    }
#else
    const bs_word lane_id = LANE_IDS;
#pragma GCC unroll 8
    for (int k = 0; k < BS_LANES; k++)
    {   bs_word b = zero + load_u64(p + (size_t)k * stride);
        BS_PIN(b);                           /* keep the broadcast a vector op */
        h |= b & (bs_word)(lane_id == (zero + (uint64_t)k));
    }
#endif
    *out = h & keep;
}

/* the same, for a group whose 8-byte lane loads would run past the end of
 * the caller's array: each hand contributes exactly its ncards bytes and
 * the rest of the lane is zero, which is the layout group_lanes produces */
static inline __attribute__((always_inline)) void
group_lanes_padded(const uint8_t *p, size_t stride, int ncards, bs_word *out)
{
    uint8_t buf[sizeof(bs_word)] = {0};
    for (int k = 0; k < BS_LANES; k++)
        memcpy(buf + 8 * k, p + (size_t)k * stride, (size_t)ncards);
    memcpy(out, buf, sizeof *out);
}

/* *hp holds one hand's card bytes per lane; out[] takes their masks */
static inline __attribute__((always_inline)) void
ids_to_mask(const bs_word *hp, int ncards, uint64_t *out)
{
    const bs_word zero = {0};
    const bs_word one = zero + 1, low6 = zero + 63;
    const bs_word h = *hp;
    bs_word m = zero;

    if (!BS_PERMUTE_EXTRACT || ncards == 8) {
#pragma GCC unroll 8
        for (int j = 0; j < ncards; j++)
            m |= one << ((h >> (8 * j)) & low6);
    } else {
        /* unrolled by hand rather than by pragma: card_byte's permute
         * indices have to be constant expressions, which a loop index is
         * not.  ncards is pinned per call site, so the tests fold. */
        if (ncards > 0) m |= one << card_byte(h, 0);
        if (ncards > 1) m |= one << card_byte(h, 1);
        if (ncards > 2) m |= one << card_byte(h, 2);
        if (ncards > 3) m |= one << card_byte(h, 3);
        if (ncards > 4) m |= one << card_byte(h, 4);
        if (ncards > 5) m |= one << card_byte(h, 5);
        if (ncards > 6) m |= one << card_byte(h, 6);
    }
    memcpy(out, &m, sizeof m);
}

static inline __attribute__((always_inline)) void
portable_masks_fixed(const uint8_t *cards, size_t stride,
                     uint64_t masks[BS_BATCH], int ncards)
{
    /* Groups whose lane loads stay inside the caller's array run first and
     * the one or two at the end run padded, rather than testing per group:
     * the caller only promises bytes up to
     * cards[(BS_BATCH-1)*stride + ncards - 1], while a group reads up to
     * (BS_LANES-1)*stride + 8 bytes past its own base. */
    const size_t total = (size_t)(BS_BATCH - 1) * stride + (size_t)ncards;
    const size_t window = (size_t)(BS_LANES - 1) * stride + 8;
    const int ngroups = BS_BATCH / BS_LANES;
    int nsafe = total >= window
                    ? (int)((total - window) / ((size_t)BS_LANES * stride)) + 1
                    : 0;
    if (nsafe > ngroups) nsafe = ngroups;

    bs_word h;
    for (int g = 0; g < nsafe; g++) {
        const uint8_t *p = cards + (size_t)g * BS_LANES * stride;
        group_lanes(p, stride, ncards, &h);
        ids_to_mask(&h, ncards, masks + (size_t)BS_LANES * g);
    }
    for (int g = nsafe; g < ngroups; g++) {
        const uint8_t *p = cards + (size_t)g * BS_LANES * stride;
        group_lanes_padded(p, stride, ncards, &h);
        ids_to_mask(&h, ncards, masks + (size_t)BS_LANES * g);
    }
}

/* The arities the games use are pinned, the rest take the scalar core --
 * the same dispatch as the instruction-set cores.  The card loop is
 * unrolled by hand, so a call site that left ncards a variable would run
 * its eight tests for real. */
void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH])
{
    switch (ncards) {
    case 4:  portable_masks_fixed(cards, stride, masks, 4); return;
    case 5:  portable_masks_fixed(cards, stride, masks, 5); return;
    case 6:  portable_masks_fixed(cards, stride, masks, 6); return;
    case 7:  portable_masks_fixed(cards, stride, masks, 7); return;
    default: masks_from_cards_scalar(cards, ncards, stride, masks); return;
    }
}
