/* Public API for the bitsliced kernels.  (Applications normally
 * want circuit_eval.h, the per-game API; this header is the plane-level
 * layer underneath it, for callers that keep planes around between calls.)
 *
 * The raw kernel interface (a circuit evaluator over 52 or 104 bit-plane
 * words) is fast, but calling it directly means re-implementing the same
 * scatter-pack into planes and the same per-bit output extraction, neither
 * of which vectorizes.  This header takes one presence mask per hand in and
 * returns one value per hand out, batch of BS_BATCH.
 *
 * Hand representation: uint64_t presence mask, bit (4*rank + suit) set when
 * that card is in the hand -- the same card ids used everywhere in the repo.
 * Value representation: the circuit's output word (order-isomorphic 24-bit
 * value, dense class id, ... whatever the linked circuit emits), zero-
 * extended to uint32_t.
 *
 * The transpose primitives underneath are exposed too, for callers that want
 * to keep planes around (equity loops that reuse a board across many
 * villains should still talk planes directly).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BS_BATCH (the batch width, one hand per bit lane), BS_LANES and the
 * bs_word vector type come from bsword.h, which the generated circuits
 * include as well. */
#include "bsword.h"

/* Link-time check that every object in the binary was compiled with the
 * same BS_BATCH.  The library (bspack.c) defines one integer whose name
 * carries its width; each translation unit that includes this header
 * keeps a reference to the symbol for the width it was compiled with.  A
 * mismatch is an undefined symbol at link time instead of arrays of two
 * different sizes at run time. */
#define BS_BATCH_SYMBOL_(n) bs_batch_width_##n
#define BS_BATCH_SYMBOL(n)  BS_BATCH_SYMBOL_(n)
extern const int BS_BATCH_SYMBOL(BS_BATCH);
static const int *const bs_batch_width_check_ __attribute__((used)) =
    &BS_BATCH_SYMBOL(BS_BATCH);

/* The primitive everything below is built on: a generic in-place 64x64
 * bit-matrix transpose, run on each u64 lane of rows[] independently
 * (BS_LANES matrices transpose at once):
 *   rows[k] lane w bit l  <-  rows[l] lane w bit k
 * The bs_transpose/bs_untranspose pair is this call plus the batch layout
 * (hand 64w+i lives in lane w) -- see bspack.c. */
void bs_transpose_64x64(bs_word rows[64]);

/* Transpose BS_BATCH presence masks into 64 bit planes:
 *   planes[c] lane bit l  =  bit c of masks[l]    (c = 0..63, l < BS_BATCH)
 * Writes all 64 planes (planes[52..63] carry the masks' unused high bits),
 * so the destination must have room for 64 words.  For the omaha layout
 * that means the input array is bs_word in[116], not in[104]: the board
 * transpose into in + 52 writes through in[115]. */
void bs_transpose(const uint64_t masks[BS_BATCH], bs_word planes[64]);

/* Same transpose, but plane for card c lands at planes[map[c]] and only
 * those 52 planes are written -- for circuits whose inputs are permuted
 * (map = bs_card_input).  map values must be < nplanes of the buffer. */
void bs_transpose_map(const uint64_t masks[BS_BATCH], const int map[52],
                      bs_word *planes);

/* Inverse transpose of the output side: gather nout output planes into one
 * value per hand,
 *   vals[l]  =  sum_k  ((planes[k] lane bit l) & 1) << k       (nout <= 32)
 */
void bs_untranspose(const bs_word *planes, int nout, uint32_t vals[BS_BATCH]);

/* Build presence masks from card lists (card id = 4*rank + suit:
 *   masks[l]  =  OR over j < ncards of  1 << cards[l*stride + j]
 * for l < BS_BATCH.  ncards must be 1..8 and stride >= ncards; a stride
 * larger than ncards lets record layouts carry other fields between hands
 * (e.g. an omaha deal stored as 5 board cards then k hole cards converts
 * with two calls, one per slice, both with stride 5+k).  Card order within
 * a hand does not matter, and duplicate ids are harmless (OR).
 * Vectorized on AVX-512 VBMI targets (strides above 16 take the scalar path
 * there too), on arm64/NEON (stride + ncards above 32 takes the scalar path
 * there), and on AVX2 (any stride); scalar everywhere else, or everywhere
 * when built with -DBS_SCALAR_MASKS (lib/Makefile MASKS=scalar).
 * -DBS_PORTABLE_MASKS (MASKS=portable) selects a generic vector core
 * instead, on any target whose vector unit has a per-element variable
 * shift.  Never reads past the last hand's last card,
 * cards[(BS_BATCH-1)*stride + ncards - 1]. */
void bs_masks_from_cards(const uint8_t *cards, int ncards, size_t stride,
                         uint64_t masks[BS_BATCH]);

/* One-call wrappers around the linked circuit (whatever bs_eval object the
 * binary links).  bs_eval_hands serves 52-input circuits: holdem value,
 * value+map composed, dense map -- anything with bs_card_input or 52
 * positional planes.  bs_eval_hands2 serves the omaha 104-plane layout
 * (hole cards -> planes 0..51, board -> planes 52..103).  Defined in
 * verify/pack/bsglue.c (weak externs, tools only), not in the
 * library -- see the note at the end of bspack.c. */
void bs_eval_hands(const uint64_t hands[BS_BATCH], uint32_t vals[BS_BATCH]);
void bs_eval_hands2(const uint64_t hole[BS_BATCH],
                    const uint64_t board[BS_BATCH], uint32_t vals[BS_BATCH]);

/* The same wrappers with the circuit passed explicitly, for binaries that
 * link several circuits under compile-time-renamed symbols (-Dbs_eval=...;
 * the bench/ harnesses).  card_input = the circuit's
 * bs_card_input, or NULL for positional planes. */
typedef void bs_eval_fn(const bs_word *in, bs_word *out);
void bs_eval_hands_fn(bs_eval_fn *eval, const int *card_input, int nout,
                      const uint64_t hands[BS_BATCH],
                      uint32_t vals[BS_BATCH]);
void bs_eval_hands2_fn(bs_eval_fn *eval, int nout,
                       const uint64_t hole[BS_BATCH],
                       const uint64_t board[BS_BATCH],
                       uint32_t vals[BS_BATCH]);

#ifdef __cplusplus
}
#endif
