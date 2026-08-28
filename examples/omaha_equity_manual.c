/* Omaha heads-up equity by enumerating every board, computed at the
 * plane level: the same result as omaha_equity.c, which uses the
 * library's fused circuit_equity_omaha call, but here the transpose,
 * the evaluator circuit, and the comparator are called separately
 * (bsapi.h).  Planes that do not change between calls are built once.
 * Build with the Makefile in this directory.
 *
 * Cards are ids 0..51, 4*rank + suit, rank 0 = deuce .. 12 = ace, suits
 * any fixed labeling 0..3; a hand is the OR of 1 << id over its cards. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "circuit_eval.h"

#define CARD(rank, suit) ((rank) * 4 + (suit))
enum { S, H, D, C };

/* Print a mask as cards, suit letters matching the enum above. */
static void print_cards(uint64_t mask)
{
    for (int id = 51; id >= 0; id--)
        if (mask >> id & 1)
            printf("%c%c ", CIRCUIT_RANK_CHAR(id / 4), "shdc"[id % 4]);
}

/* Number of set bits among the first n lanes of a plane.  Lane l is bit
 * l % 64 of u64 word l / 64. */
static uint64_t count_lanes(const bs_word *plane, int n)
{
    uint64_t total = 0;
    for (int i = 0; i < BS_LANES && n > 0; i++, n -= 64) {
        uint64_t word = (*plane)[i];
        if (n < 64)
            word &= (1ull << n) - 1;
        total += (uint64_t)__builtin_popcountll(word);
    }
    return total;
}

/* Score one batch of n boards against the two fixed holes.  The omaha
 * circuit's input is 104 planes, hole card c at [c] and board card c at
 * [52 + c]; ina and inb already hold the hole planes (see equity()).
 * The comparator reads a's 24 value planes at [0..23] and b's at
 * [24..47], so the two evaluator runs write adjacent blocks; its two
 * output planes are a > b and a == b per lane. */
static void showdown(bs_word ina[116], bs_word inb[116],
                     const uint64_t boards[BS_BATCH], int n,
                     uint64_t *wins, uint64_t *ties)
{
    bs_word bd[64], vals[48], cmp[2];
    bs_transpose(boards, bd);          /* always writes 64 planes */
    memcpy(ina + 52, bd, 52 * sizeof(bs_word));
    memcpy(inb + 52, bd, 52 * sizeof(bs_word));
    circuit_eval_omaha_raw(ina, vals);
    circuit_eval_omaha_raw(inb, vals + 24);
    circuit_cmp24_raw(vals, cmp);
    *wins += count_lanes(&cmp[0], n);
    *ties += count_lanes(&cmp[1], n);
}

/* Exact heads-up equity of 4-card hole a vs b: every 5-card board from
 * the 44 remaining cards, BS_BATCH boards per call. */
static void equity(uint64_t a, uint64_t b)
{
    uint64_t masks[BS_BATCH], bd[BS_BATCH];
    bs_word ina[116], inb[116];
    uint64_t wins = 0, ties = 0, total = 0;
    int n = 0;

    /* The holes are the same in every lane of every batch: transpose
     * each once.  bs_transpose writes 64 planes; 52..63 are overwritten
     * by the board planes in showdown(). */
    for (int l = 0; l < BS_BATCH; l++)
        masks[l] = a;
    bs_transpose(masks, ina);
    for (int l = 0; l < BS_BATCH; l++)
        masks[l] = b;
    bs_transpose(masks, inb);

    for (int c0 = 0; c0 < 52; c0++)
    for (int c1 = c0 + 1; c1 < 52; c1++)
    for (int c2 = c1 + 1; c2 < 52; c2++)
    for (int c3 = c2 + 1; c3 < 52; c3++)
    for (int c4 = c3 + 1; c4 < 52; c4++) {
        uint64_t board = 1ull << c0 | 1ull << c1 | 1ull << c2 |
                         1ull << c3 | 1ull << c4;
        if (board & (a | b))
            continue;
        bd[n] = board;
        if (++n == BS_BATCH) {
            showdown(ina, inb, bd, n, &wins, &ties);
            total += n; n = 0;
        }
    }
    if (n) {
        showdown(ina, inb, bd, n, &wins, &ties);
        total += n;
    }
    printf("boards %llu  win %llu  tie %llu  lose %llu  equity %.4f\n",
           (unsigned long long)total, (unsigned long long)wins,
           (unsigned long long)ties, (unsigned long long)(total - wins - ties),
           (wins + ties / 2.0) / total);
}

int main(void)
{
    uint64_t aakk = 1ull << CARD(CIRCUIT_RANK_A, S) | 1ull << CARD(CIRCUIT_RANK_A, H) |
                    1ull << CARD(CIRCUIT_RANK_K, S) | 1ull << CARD(CIRCUIT_RANK_K, H);
    uint64_t jt98 = 1ull << CARD(CIRCUIT_RANK_J, S) | 1ull << CARD(CIRCUIT_RANK_T, S) |
                    1ull << CARD(CIRCUIT_RANK_9, H) | 1ull << CARD(CIRCUIT_RANK_8, H);
    print_cards(aakk);
    printf("vs ");
    print_cards(jt98);
    printf("preflop: ");
    equity(aakk, jt98);
    return 0;
}
