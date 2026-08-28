/* Holdem heads-up equity: exact, by enumerating every board.  Build
 * with the Makefile in this directory.
 *
 * Cards are ids 0..51, 4*rank + suit, rank 0 = deuce .. 12 = ace, suits
 * any fixed labeling 0..3; a hand is the OR of 1 << id over its cards. */
#include <stdio.h>
#include <stdint.h>
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

/* Exact heads-up equity of hole pair a vs b: every 5-card board from the
 * 48 remaining cards, BS_BATCH boards per call. */
static void equity(uint64_t a, uint64_t b)
{
    uint64_t ha[BS_BATCH], hb[BS_BATCH], wins = 0, ties = 0, total = 0;
    int n = 0;
    for (int c0 = 0; c0 < 52; c0++)
    for (int c1 = c0 + 1; c1 < 52; c1++)
    for (int c2 = c1 + 1; c2 < 52; c2++)
    for (int c3 = c2 + 1; c3 < 52; c3++)
    for (int c4 = c3 + 1; c4 < 52; c4++) {
        uint64_t board = 1ull << c0 | 1ull << c1 | 1ull << c2 |
                         1ull << c3 | 1ull << c4;
        if (board & (a | b))
            continue;
        ha[n] = a | board;
        hb[n] = b | board;
        if (++n == BS_BATCH) {
            uint64_t w, t;
            circuit_equity_holdem(ha, hb, n, &w, &t);
            wins += w; ties += t; total += n; n = 0;
        }
    }
    if (n) {
        uint64_t w, t;
        circuit_equity_holdem(ha, hb, n, &w, &t);
        wins += w; ties += t; total += n;
    }
    printf("boards %llu  win %llu  tie %llu  lose %llu  equity %.4f\n",
           (unsigned long long)total, (unsigned long long)wins,
           (unsigned long long)ties, (unsigned long long)(total - wins - ties),
           (wins + ties / 2.0) / total);
}

int main(void)
{
    uint64_t aks = 1ull << CARD(CIRCUIT_RANK_A, S) | 1ull << CARD(CIRCUIT_RANK_K, S);
    uint64_t qq  = 1ull << CARD(CIRCUIT_RANK_Q, H) | 1ull << CARD(CIRCUIT_RANK_Q, D);
    print_cards(aks);
    printf("vs ");
    print_cards(qq);
    printf("preflop: ");
    equity(aks, qq);
    return 0;
}
