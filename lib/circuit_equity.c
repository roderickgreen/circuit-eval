/* Implementation of the circuit_equity_* calls: heads-up showdown counting
 * with the classification fused into plane space.  Each call packs both
 * players' cards into bit planes (bspack.c), runs the value circuit once
 * per player, feeds the value planes to the comparator circuit, and
 * popcounts the resulting outcome planes -- the per-hand values are never
 * transposed back out. */
#include <string.h>
#include "circuit_eval.h"

/* Population count of the first n lanes of an outcome plane.  Lane l of
 * the batch is bit l % 64 of u64 word l / 64 (bspack.c), so "first n
 * lanes" is a low-bits mask on the one partial word.  (Pointer parameter
 * rather than value: wide vectors passed by value draw a psABI note from
 * gcc on AVX-512 targets.) */
static uint64_t count_lanes(const bs_word *w, int n)
{
    uint64_t total = 0;
    for (int i = 0; i < BS_LANES && n > 0; i++, n -= 64) {
        uint64_t lane = (*w)[i];
        if (n < 64)
            lane &= (1ull << n) - 1;
        total += (uint64_t)__builtin_popcountll(lane);
    }
    return total;
}

void circuit_equity_holdem(const uint64_t a[BS_BATCH], const uint64_t b[BS_BATCH],
                           int n, uint64_t *wins, uint64_t *ties)
{
    /* The comparator reads a's value bits at planes 0..23 and b's at
     * 24..47, so the two evaluator runs write adjacent output blocks. */
    bs_word in[52], cin[48], cmp[2];
    bs_transpose_map(a, circuit_eval_holdem_raw_card_input, in);
    circuit_eval_holdem_raw(in, cin);
    bs_transpose_map(b, circuit_eval_holdem_raw_card_input, in);
    circuit_eval_holdem_raw(in, cin + 24);
    circuit_cmp24_raw(cin, cmp);
    *wins = count_lanes(&cmp[0], n);
    *ties = count_lanes(&cmp[1], n);
}

/* Pack the shared-board omaha input layout for both players: hole card c
 * at plane [c], board card c at plane [52 + c].  bs_transpose always
 * writes 64 planes, so the order and sizes matter: each hole's planes
 * 52..63 are overwritten by the board's, the board's planes beyond 103
 * land in ina's unused tail (hence 116), and the board is transposed
 * once and copied into the second buffer. */
static void pack_pair(const uint64_t hole_a[BS_BATCH], const uint64_t hole_b[BS_BATCH],
                      const uint64_t board[BS_BATCH],
                      bs_word ina[116], bs_word inb[104])
{
    bs_transpose(hole_a, ina);
    bs_transpose(hole_b, inb);
    bs_transpose(board, ina + 52);
    memcpy(inb + 52, ina + 52, 52 * sizeof(bs_word));
}

void circuit_equity_omaha(const uint64_t hole_a[BS_BATCH],
                          const uint64_t hole_b[BS_BATCH],
                          const uint64_t board[BS_BATCH],
                          int n, uint64_t *wins, uint64_t *ties)
{
    bs_word ina[116], inb[104], cin[48], cmp[2];
    pack_pair(hole_a, hole_b, board, ina, inb);
    circuit_eval_omaha_raw(ina, cin);
    circuit_eval_omaha_raw(inb, cin + 24);
    circuit_cmp24_raw(cin, cmp);
    *wins = count_lanes(&cmp[0], n);
    *ties = count_lanes(&cmp[1], n);
}

void circuit_equity_omaha_hilo(const uint64_t hole_a[BS_BATCH],
                               const uint64_t hole_b[BS_BATCH],
                               const uint64_t board[BS_BATCH], int n,
                               uint64_t *hi_wins, uint64_t *hi_ties,
                               uint64_t *lo_wins, uint64_t *lo_ties,
                               uint64_t *no_low)
{
    bs_word ina[116], inb[104], cin[48], hi[2], lo[2];
    pack_pair(hole_a, hole_b, board, ina, inb);

    circuit_eval_omaha_raw(ina, cin);
    circuit_eval_omaha_raw(inb, cin + 24);
    circuit_cmp24_raw(cin, hi);

    /* The 8-bit low values feed cmp8 directly: a at planes 0..7, b at
     * 8..15, matching the two evaluator output blocks. */
    circuit_eval_omaha_low_raw(ina, cin);
    circuit_eval_omaha_low_raw(inb, cin + 8);
    circuit_cmp8_raw(cin, lo);

    /* Smaller low value = stronger, 0xFF = no qualifying low
     * (circuit_eval.h), so on the low comparator gt means b takes the
     * low outright (better low, or the only qualifying one) and the
     * neither-gt-nor-eq lanes are a's.  eq covers both a genuine split
     * and the nobody-qualifies case; the two are separated by whether
     * a's value is all-ones (eq then forces b's to match). */
    bs_word all_a = cin[0];
    for (int i = 1; i < 8; i++)
        all_a &= cin[i];
    bs_word nl = lo[1] & all_a;

    /* Low-half-of-the-pot semantics: when no low qualifies, the low half
     * follows the high result (the high winner scoops, a high tie splits
     * everything). */
    bs_word lw = (~lo[0] & ~lo[1]) | (nl & hi[0]);
    bs_word lt = (lo[1] & ~nl) | (nl & hi[1]);

    *hi_wins = count_lanes(&hi[0], n);
    *hi_ties = count_lanes(&hi[1], n);
    *lo_wins = count_lanes(&lw, n);
    *lo_ties = count_lanes(&lt, n);
    *no_low = count_lanes(&nl, n);
}
