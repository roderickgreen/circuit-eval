/* Category census: evaluates every 7-card holdem hand, all C(52,7) =
 * 133,784,560 of them, and prints how many fall in each category.  The
 * counts are the standard 7-card frequency table.  Build with the
 * Makefile in this directory.
 *
 * Hands are enumerated as records of 7 card ids (card id = 4*rank +
 * suit), converted to presence masks a batch at a time with
 * bs_masks_from_cards, and scored BS_BATCH at a time; the final partial
 * batch is padded with zero records whose values are discarded. */
#include <stdio.h>
#include <stdint.h>
#include "circuit_eval.h"

int main(void)
{
    static const char *const names[10] = {
        "", "high card", "one pair", "two pair", "trips", "straight",
        "flush", "full house", "quads", "straight flush"};
    static uint8_t cards[BS_BATCH][7];
    uint64_t hands[BS_BATCH];
    uint32_t vals[BS_BATCH];
    uint64_t count[10] = {0};
    int n = 0;
    for (int c0 = 0; c0 < 52; c0++)
    for (int c1 = c0 + 1; c1 < 52; c1++)
    for (int c2 = c1 + 1; c2 < 52; c2++)
    for (int c3 = c2 + 1; c3 < 52; c3++)
    for (int c4 = c3 + 1; c4 < 52; c4++)
    for (int c5 = c4 + 1; c5 < 52; c5++)
    for (int c6 = c5 + 1; c6 < 52; c6++) {
        uint8_t *hand = cards[n++];
        hand[0] = c0; hand[1] = c1; hand[2] = c2; hand[3] = c3;
        hand[4] = c4; hand[5] = c5; hand[6] = c6;
        if (n == BS_BATCH) {
            bs_masks_from_cards(&cards[0][0], 7, 7, hands);
            circuit_eval_holdem(hands, vals);
            for (int i = 0; i < n; i++)
                count[CIRCUIT_CATEGORY(vals[i])]++;
            n = 0;
        }
    }
    if (n) {
        for (int i = n; i < BS_BATCH; i++)
            for (int j = 0; j < 7; j++)
                cards[i][j] = 0;
        bs_masks_from_cards(&cards[0][0], 7, 7, hands);
        circuit_eval_holdem(hands, vals);
        for (int i = 0; i < n; i++)
            count[CIRCUIT_CATEGORY(vals[i])]++;
    }

    uint64_t total = 0;
    for (int cat = 9; cat >= 1; cat--) {
        printf("%-15s %11llu\n", names[cat], (unsigned long long)count[cat]);
        total += count[cat];
    }
    printf("%-15s %11llu\n", "total", (unsigned long long)total);
    return 0;
}
