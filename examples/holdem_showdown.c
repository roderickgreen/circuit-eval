/* Holdem showdown: two hole pairs on one board.  Evaluates both hands,
 * prints the five cards each one plays, and names the winner.  Build
 * with the Makefile in this directory.
 *
 * Cards are ids 0..51, 4*rank + suit, rank 0 = deuce .. 12 = ace, suits
 * any fixed labeling 0..3; a hand is the OR of 1 << id over its cards.
 * The value of a 7-card hand is the value of its best 5-card subset, and
 * the holdem evaluator accepts 5-card masks, so the cards that play are
 * found by evaluating every 5-card subset and keeping one whose value
 * matches. */
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

/* Value of one 7-card hand, plus the 5-card subset that makes it.  Lane 0
 * holds the whole hand, lanes 1..21 its 5-card subsets; the remaining
 * lanes are zero and unread. */
static uint32_t evaluate(uint64_t hand, uint64_t *plays)
{
    uint64_t hands[BS_BATCH] = {hand};
    uint32_t vals[BS_BATCH];
    int n = 1;
    for (int a = 0; a < 52; a++)
        if (hand >> a & 1)
            for (int b = a + 1; b < 52; b++)
                if (hand >> b & 1)
                    hands[n++] = hand & ~(1ull << a) & ~(1ull << b);
    circuit_eval_holdem(hands, vals);
    *plays = 0;
    for (int i = 1; i < n; i++)
        if (vals[i] == vals[0]) {
            *plays = hands[i];
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
        1ull << CARD(CIRCUIT_RANK_A, S) | 1ull << CARD(CIRCUIT_RANK_K, S),
        1ull << CARD(CIRCUIT_RANK_7, H) | 1ull << CARD(CIRCUIT_RANK_5, S)};
    uint32_t val[2];

    printf("board     ");
    print_cards(board);
    printf("\n");
    for (int p = 0; p < 2; p++) {
        uint64_t plays;
        val[p] = evaluate(hole[p] | board, &plays);
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
