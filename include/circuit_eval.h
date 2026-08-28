/* The evaluator library's public API: one call per game, batch of BS_BATCH.
 *
 * The API is batch-only.  Every call takes BS_BATCH hands and returns
 * BS_BATCH values; there is no single-hand entry point, because a call
 * costs the same whether one lane or every lane holds a real hand.  To
 * score one hand, put it in lane 0, leave the rest zero, and read
 * vals[0].
 *
 * Hand representation: uint64_t presence mask, bit (4*rank + suit) set when
 * that card is in the hand.  rank 0 = deuce .. 12 = ace; the four suits are
 * interchangeable labels 0..3.  Hands that arrive as lists of card ids
 * convert with bs_masks_from_cards (bsapi.h), one batch per call.
 *
 * High value representation (holdem and omaha high): the 24-bit
 * order-isomorphic encoding, bigger = stronger, equal = tie --
 *   value = category << 20 | kicker nibbles, one nibble per rank,
 *           left-justified from bit 16
 * with categories 1 HighCard .. 9 StraightFlush.  The encoding is defined
 * in docs/ENCODING.md.
 *
 * Low value representation (omaha hi-lo): 8-bit rank mask of the best
 * 8-or-better low, bit 0 = ace .. bit 7 = eight, SMALLER = stronger
 * (fewer/lower ranks), 0xFF = no qualifying low.
 *
 * Calls are stateless and each evaluates one batch of BS_BATCH hands
 * (bsword.h; an architectural constant -- 512 when compiled for AVX-512,
 * 128 on NEON, 256 elsewhere): masks in, one value per hand out, same
 * index.  Internally
 * each call transposes the masks into bit planes, runs the corresponding
 * *_raw circuit once (BS_BATCH hands in parallel, one gate = one vector
 * op), and transposes the outputs back.
 * Callers that keep planes around (e.g. equity loops reusing a board)
 * can skip the pack/unpack and talk to the raw circuits through bsapi.h.
 *
 * circuit_eval_omaha serves 4-, 5- and 6-card omaha with the same circuit:
 * put however many hole cards in the hole mask.  The value is the maximum
 * over every two-hole/three-board split, so any board of 3 or more cards
 * is defined and a flop or turn evaluates the current best hand.  The
 * exhaustive verification in verify/ covers 5-card boards only; shorter
 * boards follow from the same circuit but have no shipped exhaustive gate.
 */
#pragma once
#include <stdint.h>
#include "bsapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Decoding high values ------------------------------------------------
 *
 * CIRCUIT_CATEGORY pulls the hand category out of a high value;
 * CIRCUIT_RANK(v, i) pulls the i-th tie-break rank nibble, i = 0 leftmost.
 * Ranks are 0 = deuce .. 12 = ace (constants below); what each nibble
 * means depends on the category:
 *
 *   category                   rank nibbles 0..4
 *   1 CIRCUIT_HIGH_CARD        the five ranks, descending
 *   2 CIRCUIT_ONE_PAIR         pair, kicker, kicker, kicker, 0
 *   3 CIRCUIT_TWO_PAIR         high pair, low pair, kicker, 0, 0
 *   4 CIRCUIT_TRIPS            trips, kicker, kicker, 0, 0
 *   5 CIRCUIT_STRAIGHT         high rank (wheel = the five), 0, 0, 0, 0
 *   6 CIRCUIT_FLUSH            the five ranks, descending
 *   7 CIRCUIT_FULL_HOUSE       trips, pair, 0, 0, 0
 *   8 CIRCUIT_QUADS            quads, kicker, 0, 0, 0
 *   9 CIRCUIT_STRAIGHT_FLUSH   high rank (ace-to-five = the five), 0, 0, 0, 0
 *
 * Example: a pair of aces with K T 5 kickers is 0x2cb830 --
 *   CIRCUIT_CATEGORY(v) == CIRCUIT_ONE_PAIR
 *   CIRCUIT_RANK(v, 0) == CIRCUIT_RANK_A                      the pair
 *   CIRCUIT_RANK(v, 1) / (v, 2) / (v, 3) == the kickers, K then T then 5
 *   CIRCUIT_RANK_CHAR(CIRCUIT_RANK(v, 1)) == 'K'
 *
 * The nibbles after the ones the category uses are 0, and 0 is also
 * CIRCUIT_RANK_2, so a 0 nibble is not an end marker: take the count
 * from the category (CIRCUIT_NUM_RANKS) and read exactly that many.
 *
 * A low value needs no decoding: it is the rank mask of the low itself
 * (bit 0 = ace .. bit 7 = eight), except for CIRCUIT_NO_LOW.
 */

#define CIRCUIT_HIGH_CARD      1
#define CIRCUIT_ONE_PAIR       2
#define CIRCUIT_TWO_PAIR       3
#define CIRCUIT_TRIPS          4
#define CIRCUIT_STRAIGHT       5
#define CIRCUIT_FLUSH          6
#define CIRCUIT_FULL_HOUSE     7
#define CIRCUIT_QUADS          8
#define CIRCUIT_STRAIGHT_FLUSH 9

#define CIRCUIT_CATEGORY(v) ((int)((uint32_t)(v) >> 20))
#define CIRCUIT_RANK(v, i)  ((int)(((v) >> (16 - 4 * (i))) & 0xF))

/* number of meaningful rank nibbles per category, indexed by category
 * (the table above); index 0 is unused */
static const int CIRCUIT_NUM_RANKS[10] = {0, 5, 4, 3, 3, 1, 5, 2, 2, 1};

#define CIRCUIT_RANK_2 0
#define CIRCUIT_RANK_3 1
#define CIRCUIT_RANK_4 2
#define CIRCUIT_RANK_5 3
#define CIRCUIT_RANK_6 4
#define CIRCUIT_RANK_7 5
#define CIRCUIT_RANK_8 6
#define CIRCUIT_RANK_9 7
#define CIRCUIT_RANK_T 8
#define CIRCUIT_RANK_J 9
#define CIRCUIT_RANK_Q 10
#define CIRCUIT_RANK_K 11
#define CIRCUIT_RANK_A 12

/* rank -> the usual one-character name */
#define CIRCUIT_RANK_CHAR(r) ("23456789TJQKA"[(r)])

/* low value when no 8-or-better low exists */
#define CIRCUIT_NO_LOW 0xFF

/* holdem: 7-card hands (any 5 of 7), 24-bit high value */
void circuit_eval_holdem(const uint64_t hands[BS_BATCH], uint32_t vals[BS_BATCH]);

/* omaha high: exactly 2 of 4/5/6 hole cards + exactly 3 of 5 board cards */
void circuit_eval_omaha(const uint64_t hole[BS_BATCH], const uint64_t board[BS_BATCH],
                        uint32_t vals[BS_BATCH]);

/* omaha hi-lo: high as above, plus the 8-or-better low (0xFF = no low);
 * one shared input transpose feeds both circuits */
void circuit_eval_omaha_hilo(const uint64_t hole[BS_BATCH],
                             const uint64_t board[BS_BATCH],
                             uint32_t high[BS_BATCH], uint32_t low[BS_BATCH]);

/* Fused heads-up equity counters.  Each call evaluates both players'
 * hands and classifies the showdown without leaving plane space: the
 * value planes feed the comparator circuit directly and only the
 * resulting per-lane win/tie bits are counted.  Intended as the inner
 * step of an equity enumeration -- pack one matchup per lane (typically
 * successive enumerated boards), call, and sum the returned counts.
 *
 * n is the number of valid lanes (1..BS_BATCH); a tail batch's unused
 * lanes may hold anything and are not counted.  wins counts lanes where
 * player a beats player b, ties where they split; b's wins are
 * n - wins - ties.
 *
 * For holdem, a and b are full 7-card masks (hole and board cards OR'd
 * together, as in circuit_eval_holdem).  For omaha the players share
 * one board array, packed once; hole_a and hole_b are hole masks as
 * for circuit_eval_omaha (4, 5 or 6 cards). */
void circuit_equity_holdem(const uint64_t a[BS_BATCH], const uint64_t b[BS_BATCH],
                           int n, uint64_t *wins, uint64_t *ties);
void circuit_equity_omaha(const uint64_t hole_a[BS_BATCH],
                          const uint64_t hole_b[BS_BATCH],
                          const uint64_t board[BS_BATCH],
                          int n, uint64_t *wins, uint64_t *ties);

/* Omaha hi-lo counts each half of the pot separately.  hi_wins/hi_ties
 * are the high half, as above.  The low-side counts award the LOW HALF
 * of the pot rather than the low race itself: when no low qualifies the
 * high result takes that half too (the scoop), so lo_wins counts lanes
 * where a receives the whole low half (best qualifying low, or no
 * qualifying low and a wins high) and lo_ties lanes where it splits
 * (equal qualifying lows, or no qualifying low and a high tie).
 * no_low counts lanes where neither player qualifies -- informational,
 * those lanes are already folded into the low-side counts.  This makes
 * the counts sufficient for exact equity:
 *   equity_a = (hi_wins + hi_ties/2 + lo_wins + lo_ties/2) / (2*n)  */
void circuit_equity_omaha_hilo(const uint64_t hole_a[BS_BATCH],
                               const uint64_t hole_b[BS_BATCH],
                               const uint64_t board[BS_BATCH], int n,
                               uint64_t *hi_wins, uint64_t *hi_ties,
                               uint64_t *lo_wins, uint64_t *lo_ties,
                               uint64_t *no_low);

/* The raw circuit evaluators (generated C, one statement per gate; see
 * lib/Makefile regen).  Planes in, planes out, no pack/unpack:
 *   holdem     52 planes (card c at [card_input[c]]) -> 24 value planes
 *   omaha      104 planes (hole c at [c], board c at [52+c]) -> 24
 *   omaha low  same 104 planes -> 8 low planes
 */
void circuit_eval_holdem_raw(const bs_word *in, bs_word *out);
void circuit_eval_omaha_raw(const bs_word *in, bs_word *out);
void circuit_eval_omaha_low_raw(const bs_word *in, bs_word *out);

/* Value comparators, same plane-in/plane-out form, for fusing showdown
 * classification into bitsliced equity loops: per lane, gt = (a > b) and
 * eq = (a == b) as unsigned integers.  With the order-isomorphic high
 * values that is win/tie/loss directly (lose = neither).  cmp24 matches
 * the 24-bit high value width; cmp8 matches the 8-bit low value, where
 * smaller = stronger, so gt means b holds the better low.  Plane-space
 * only on purpose: scalar callers should just compare the uint32_t
 * values.
 *   cmp8       16 planes (a bit i at [i], b bit i at [8+i]) -> gt, eq
 *   cmp24      48 planes (a bit i at [i], b bit i at [24+i]) -> gt, eq
 */
void circuit_cmp8_raw(const bs_word *in, bs_word *out);
void circuit_cmp24_raw(const bs_word *in, bs_word *out);

extern const int circuit_eval_holdem_raw_nb;
extern const int circuit_eval_holdem_raw_num_outputs;
extern const int circuit_eval_holdem_raw_card_input[52];
extern const int circuit_eval_omaha_raw_nb;
extern const int circuit_eval_omaha_raw_num_outputs;
extern const int circuit_eval_omaha_raw_num_inputs;
extern const int circuit_eval_omaha_low_raw_nb;
extern const int circuit_eval_omaha_low_raw_num_outputs;
extern const int circuit_eval_omaha_low_raw_num_inputs;
extern const int circuit_cmp8_raw_nb;
extern const int circuit_cmp8_raw_num_outputs;
extern const int circuit_cmp8_raw_num_inputs;
extern const int circuit_cmp24_raw_nb;
extern const int circuit_cmp24_raw_num_outputs;
extern const int circuit_cmp24_raw_num_inputs;

#ifdef __cplusplus
}
#endif
