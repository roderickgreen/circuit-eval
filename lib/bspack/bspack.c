/* Batch conversion layer for the bitsliced kernels -- the implementation
 * of bsapi.h.  One translation unit, split across files by function and
 * instruction set; every compile-target decision is made right here in
 * the #if blocks below, so "which code runs on my machine" is answerable
 * from this file alone.
 *
 * The pieces, one implementation per file:
 *
 *   transpose64_portable.h   bs_transpose_64x64, the in-place 64x64
 *   transpose64_neon.h       bit-matrix transpose primitive, plus the
 *                            masks and swap macros the pack core in the
 *                            same dialect builds on.  Portable GNU-vector
 *                            code and arm64/NEON respectively; the
 *                            algorithm is described at the top of
 *                            transpose64_portable.h.
 *
 *   pack_avx2.h              bs_transpose / bs_transpose_map /
 *   pack_avx512.h            bs_untranspose, one fused schedule per call:
 *   pack_neon.h              the batch gather and the plane/value scatter
 *   pack_portable.h          fold into the transpose's own passes rather
 *                            than bracketing it.  Each file's comment
 *                            explains its scheme; pack_portable.h is the
 *                            one that compiles everywhere, and is the
 *                            fallback wherever no other matches.
 *
 *   masks_from_cards_*.h     bs_masks_from_cards: a scalar core that
 *                            always compiles (the vector cores fall back
 *                            to it for shapes they refuse), one vector
 *                            core per instruction set, and a portable
 *                            vector core selected only by MASKS=portable.
 *
 * The batch layout every pack core has to bridge: a batch is BS_BATCH
 * contiguous per-hand u64s, but the lane-parallel transpose wants hand
 * 64w+i in lane w of row i -- a stride-64 gather on the way in, and its
 * inverse on the way out, where values also narrow to u32.
 *
 * Buffers handed to bs_eval must be aligned to the bs_word size (the
 * generated circuit uses aligned vector loads); the mask/value arrays
 * only need natural alignment.
 */
/* The core files under bspack/ are sections of this translation unit,
 * not headers: each refuses to compile unless this macro is defined. */
#define BSPACK_INTERNAL 1
#include <string.h>
#include "bsapi.h"

/* Which file implements bs_transpose / bs_transpose_map / bs_untranspose:
 * exactly one of BS_PACK_AVX512 / BS_PACK_AVX2 / BS_PACK_NEON /
 * BS_PACK_PORTABLE is 1.  BS_PORTABLE_PACK (lib/Makefile PACK=portable)
 * forces pack_portable.h even where a per-ISA core exists, without
 * touching -march: the intrinsics-vs-generic-vector-ops A/B, measurable
 * on any target. */

/* pack_avx512.h needs 512-bit lanes and the GFNI/VBMI permute set;
 * anything else (including BS_BATCH=512 pinned on narrow hardware, the
 * cross-testing device) keeps pack_portable.h */
#if !defined(BS_PORTABLE_PACK) && BS_BATCH == 512 && \
    defined(__AVX512F__) && defined(__AVX512BW__) && \
    defined(__AVX512VBMI__) && defined(__GFNI__)
#define BS_PACK_AVX512 1
#else
#define BS_PACK_AVX512 0
#endif

/* pack_avx2.h serves 4-lane (256-bit) words on AVX2 targets */
#if !defined(BS_PORTABLE_PACK) && !BS_PACK_AVX512 && BS_LANES == 4 && \
    defined(__AVX2__) && !defined(__aarch64__)
#define BS_PACK_AVX2 1
#else
#define BS_PACK_AVX2 0
#endif

/* pack_neon.h serves arm64 (transpose64_neon.h below admits 128/256 only) */
#if defined(__aarch64__) && !defined(BS_PORTABLE_PACK)
#define BS_PACK_NEON 1
#else
#define BS_PACK_NEON 0
#endif

#define BS_PACK_PORTABLE (!BS_PACK_AVX512 && !BS_PACK_AVX2 && !BS_PACK_NEON)

/* Which file implements bs_masks_from_cards.  Two knobs force a core
 * even where an instruction-set one exists, for A/B measurement anywhere:
 * BS_SCALAR_MASKS (lib/Makefile MASKS=scalar) and BS_PORTABLE_MASKS
 * (MASKS=portable).  The portable vector core is a knob rather than the
 * fallback because it needs a per-element variable shift to be worth
 * anything -- masks_from_cards_portable.h says what happens without one --
 * so targets with no instruction-set core keep the scalar one by
 * default. */
#if defined(BS_SCALAR_MASKS)
#define BS_MASKS_SCALAR 1
#elif defined(BS_PORTABLE_MASKS)
#define BS_MASKS_PORTABLE 1
#elif defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VBMI__)
#define BS_MASKS_AVX512 1
#elif defined(__aarch64__)
#define BS_MASKS_NEON 1
#elif defined(__AVX2__)
#define BS_MASKS_AVX2 1
#else
#define BS_MASKS_SCALAR 1   /* no wide vector unit at all */
#endif

/* ---- register fence ----------------------------------------------------- */

/* Whether a bs_word is one register on this target; wider words are
 * emulated.  __BIGGEST_ALIGNMENT__ is the widest vector width wherever the
 * target has vectors at all, except under clang on x86-64, where it is 16
 * at every -march.  BS_PIN(v) fences a vector value: the compiler neither
 * re-lowers it through scalar registers nor folds it on paths where its
 * inputs are constants.  It is a no-op on an emulated word, where the
 * register constraint could not be met.  Defined before the transpose
 * core, whose swap macros use it. */
#if defined(__clang__) && defined(__x86_64__)
#if defined(__AVX512BW__)
#define BS_VEC_BYTES 64
#elif defined(__AVX2__)
#define BS_VEC_BYTES 32
#else
#define BS_VEC_BYTES 16
#endif
#else
#define BS_VEC_BYTES __BIGGEST_ALIGNMENT__
#endif
#define BS_WORD_IN_REG ((BS_BATCH / 8) <= BS_VEC_BYTES)
#if BS_WORD_IN_REG
#define BS_PIN(v) __asm__("" : "+x"(v))
#else
#define BS_PIN(v) ((void)0)
#endif

/* ---- transpose core ----------------------------------------------------- */

#if BS_PACK_NEON
#include "transpose64_neon.h"
#else
#include "transpose64_portable.h"
#endif

/* ---- the pack API: masks -> planes -> vals ------------------------------ */

#if BS_PACK_AVX512
#include "pack_avx512.h"
#elif BS_PACK_AVX2
#include "pack_avx2.h"
#elif BS_PACK_NEON
#include "pack_neon.h"
#else
#include "pack_portable.h"
#endif /* pack API selection */

/* ---- presence masks from card lists ------------------------------------- */

/* bs_masks_from_cards (bsapi.h): masks[l] = OR of 1 << cards[l*stride + j]
 * over j < ncards.  A vector core loads a fixed window around each group
 * of hands, so the batch's last hands are first copied into a zero-padded
 * local buffer -- no load ever touches memory past the caller's array, as
 * the header promises. */

#include "masks_from_cards_scalar.h"
#if defined(BS_MASKS_AVX512)
#include "masks_from_cards_avx512.h"
#elif defined(BS_MASKS_NEON)
#include "masks_from_cards_neon.h"
#elif defined(BS_MASKS_AVX2)
#include "masks_from_cards_avx2.h"
#elif defined(BS_MASKS_PORTABLE)
#include "masks_from_cards_portable.h"
#endif

/* The batch-width symbol bsapi.h references from every translation unit
 * (see the link-time check there). */
const int BS_BATCH_SYMBOL(BS_BATCH) = BS_BATCH;

/* ---- one-call wrappers ------------------------------------------------- */

void bs_eval_hands_fn(bs_eval_fn *eval, const int *card_input, int nout,
                      const uint64_t hands[BS_BATCH], uint32_t vals[BS_BATCH])
{
    bs_word in[64], out[32];
    if (card_input)
        bs_transpose_map(hands, card_input, in);
    else
        bs_transpose(hands, in);
    eval(in, out);
    bs_untranspose(out, nout, vals);
}

void bs_eval_hands2_fn(bs_eval_fn *eval, int nout,
                       const uint64_t hole[BS_BATCH],
                       const uint64_t board[BS_BATCH], uint32_t vals[BS_BATCH])
{
    bs_word in[116], out[32];   /* 52 hole + 64 board (12 junk planes) */
    bs_transpose(hole, in);     /* writes in[0..63]; 52..63 overwritten */
    bs_transpose(board, in + 52);
    eval(in, out);
    bs_untranspose(out, nout, vals);
}

/* The link-time-bound wrappers (bs_eval_hands / bs_eval_hands2, which call
 * whatever bs_eval was linked in) live in verify/pack/bsglue.c, not
 * here: their weak bs_* externs would force -Wl,-U flags on every macOS
 * program that links this library.  circuit_eval.c uses the _fn forms
 * above and names its circuits explicitly. */
