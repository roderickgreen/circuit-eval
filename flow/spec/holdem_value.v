// Holdem evaluator, order-isomorphic 24-bit value, one circuit exact
// for 5, 6, or 7 cards.
// Per-rank counters + per-suit flush detect feed the rank-side and
// flush-side value modules, and a final mux picks the side. This
// exploits the fact that with no more than 7 cards, a hand cannot
// score as a flush and as a full house or quads at the same time. An
// input with more than 7 bits set will not evaluate correctly.
//
// Input is rank-major: cards[r][s] = card of rank r, suit s is held
// (rank 0 = deuce .. 12 = ace), so cards[r] is the 4-suit group of one
// rank; a suit-major transpose (suit_view.v) feeds the flush detect.
// value = the 24-bit value, comparable as an integer.

module holdem_value (
  input  [12:0][3:0] cards,
  output [23:0]      value
);
  `include "lib.vh"

  // rank counters: how many of each rank
  reg [12:0][2:0] counts;
  integer r, s;
  always @* begin
    for (r = 0; r < 13; r = r + 1)
      counts[r] = popcnt4(cards[r]);
  end

  // per-suit rank masks; a suit with 5+ cards flushes (7 cards: at most one)
  wire [3:0][12:0] by_suit;
  suit_view SV (.by_rank(cards), .by_suit(by_suit));
  reg [3:0] suit_ge5;
  reg [12:0] flush_mask;
  always @* begin
    for (s = 0; s < 4; s = s + 1)
      suit_ge5[s] = popcnt13(by_suit[s]) >= 5;
    flush_mask = 13'd0;
    for (s = 0; s < 4; s = s + 1)
      if (suit_ge5[s])                    // the flushing suit's rank mask
        flush_mask = flush_mask | by_suit[s];
  end
  wire any_flush = |suit_ge5;

  wire [23:0] rank_val, flush_val;
  rank_value  RV (.counts(counts),   .value(rank_val));
  flush_value FV (.mask(flush_mask), .value(flush_val));

  assign value = any_flush ? flush_val : rank_val;
endmodule
