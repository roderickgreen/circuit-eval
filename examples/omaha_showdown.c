/* Omaha showdown: two 4-card holdings on one board.  Evaluates both
 * hands, prints the five cards each one plays, and names the winner.
 * Build with the Makefile in this directory.
 *
 * Cards are ids 0..51, 4*rank + suit, rank 0 = deuce .. 12 = ace, suits
 * any fixed labeling 0..3; a hand is the OR of 1 << id over its cards.
 * Hole and board masks stay separate: the omaha evaluator applies the
 * two-from-hole, three-from-board rule itself, and its value is the
 * value of the best such 5-card split.  The holdem evaluator accepts
 * 5-card masks, so the cards that play are found by evaluating every
 * legal split with it and keeping one whose value matches. */
#include <stdio.h>
#include <stdint.h>
#include "circuit_eval.h"

#define CARD(rank, suit) ((rank) * 4 + (suit))
enum { S, H, D, C };

static const char *const category_name[10] = {
    "", "high card", "one pair", "two pair", "trips", "straight",
    "flush", "full house", "quads", "straight flush"};

/* Print a mask as cards, suit letters matching the enum above. */
static void print_cards(uint64_t mask)
{
    for (int id = 51; id >= 0; id--)
        if (mask >> id & 1)
            printf("%c%c ", CIRCUIT_RANK_CHAR(id / 4), "shdc"[id % 4]);
}

/* Append every k-card subset of mask to out[], returning the new count. */
static int subsets(uint64_t mask, int k, uint64_t *out, int n, uint64_t acc, int from)
{
    if (k == 0) {
        out[n++] = acc;
        return n;
    }
    for (int id = from; id < 52; id++)
        if (mask >> id & 1)
            n = subsets(mask, k - 1, out, n, acc | 1ull << id, id + 1);
    return n;
}

/* Value of one omaha hand, plus the 5-card split that makes it. */
static uint32_t evaluate(uint64_t hole, uint64_t board, uint64_t *plays)
{
    uint64_t holes[BS_BATCH] = {hole}, boards[BS_BATCH] = {board};
    uint64_t twos[16], threes[16], splits[BS_BATCH] = {0};
    uint32_t vals[BS_BATCH], split_vals[BS_BATCH];
    int n2 = subsets(hole, 2, twos, 0, 0, 0);
    int n3 = subsets(board, 3, threes, 0, 0, 0);
    int n = 0;
    for (int i = 0; i < n2; i++)
        for (int j = 0; j < n3; j++)
            splits[n++] = twos[i] | threes[j];

    circuit_eval_omaha(holes, boards, vals);
    circuit_eval_holdem(splits, split_vals);
    *plays = 0;
    for (int i = 0; i < n; i++)
        if (split_vals[i] == vals[0]) {
            *plays = splits[i];
            break;
        }
    return vals[0];
}

int main(void)
{
    uint64_t board = 1ull << CARD(CIRCUIT_RANK_A, D) | 1ull << CARD(CIRCUIT_RANK_K, D) |
                     1ull << CARD(CIRCUIT_RANK_7, C) | 1ull << CARD(CIRCUIT_RANK_7, D) |
                     1ull << CARD(CIRCUIT_RANK_2, H);
    uint64_t hole[2] = {
        1ull << CARD(CIRCUIT_RANK_A, S) | 1ull << CARD(CIRCUIT_RANK_A, H) |
        1ull << CARD(CIRCUIT_RANK_K, S) | 1ull << CARD(CIRCUIT_RANK_K, H),
        1ull << CARD(CIRCUIT_RANK_7, H) | 1ull << CARD(CIRCUIT_RANK_7, S) |
        1ull << CARD(CIRCUIT_RANK_Q, S) | 1ull << CARD(CIRCUIT_RANK_J, S)};
    uint32_t val[2];

    printf("board     ");
    print_cards(board);
    printf("\n");
    for (int p = 0; p < 2; p++) {
        uint64_t plays;
        val[p] = evaluate(hole[p], board, &plays);
        printf("player %d  ", p + 1);
        print_cards(hole[p]);
        printf("  plays ");
        print_cards(plays);
        printf(" %s\n", category_name[CIRCUIT_CATEGORY(val[p])]);
    }
    if (val[0] == val[1])
        printf("tie\n");
    else
        printf("player %d wins\n", val[0] > val[1] ? 1 : 2);
    return 0;
}
