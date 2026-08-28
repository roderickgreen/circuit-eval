// Omaha high evaluator, order-isomorphic 24-bit value.
// The hand must use exactly 2 hole + 3 board cards, so every
// category is an exists/max over per-rank
// (hole_count, board_count) splits. One circuit is exact for 4, 5 or 6
// hole cards (PLO4/5/6) because no predicate references the total counts.
//
// Inputs are rank-major: hole[r][s] / board[r][s] = hole / board holds
// the card of rank r, suit s (rank 0 = deuce .. 12 = ace), so hole[r]
// is the 4-suit group of one rank.  value = cat<<20 | n1..n5 nibbles,
// the same encoding as the holdem evaluator, comparable as an integer.
//
// The evaluator is factored by side.  Cards enter only through
// per-side statistics: omaha_hole_stats and omaha_board_stats each
// summarize one side's cards alone, and omaha_value_core combines the
// two summaries into the value word without seeing a card.  This
// follows the mandatory 2+3 split: every predicate joins a
// threshold on one side's counts with a threshold on the other's.

module omaha_value (
  input  [12:0][3:0] hole,
  input  [12:0][3:0] board,
  output [23:0]      value
);
  wire [12:0]      hole_ge1, hole_ge2;
  wire [3:0]       hole_flush2;
  wire [3:0][12:0] hole_by_suit;
  wire [12:0]      board_ge1, board_ge2, board_ge3;
  wire [3:0]       board_flush3;
  wire [3:0][12:0] board_by_suit;

  omaha_hole_stats HS (
    .hole    (hole),
    .ge1     (hole_ge1),
    .ge2     (hole_ge2),
    .flush2  (hole_flush2),
    .by_suit (hole_by_suit)
  );

  omaha_board_stats BS (
    .board   (board),
    .ge1     (board_ge1),
    .ge2     (board_ge2),
    .ge3     (board_ge3),
    .flush3  (board_flush3),
    .by_suit (board_by_suit)
  );

  omaha_value_core CORE (
    .hole_ge1      (hole_ge1),
    .hole_ge2      (hole_ge2),
    .hole_flush2   (hole_flush2),
    .hole_by_suit  (hole_by_suit),
    .board_ge1     (board_ge1),
    .board_ge2     (board_ge2),
    .board_ge3     (board_ge3),
    .board_flush3  (board_flush3),
    .board_by_suit (board_by_suit),
    .value         (value)
  );
endmodule

// Statistics of the hole cards alone, capped at what a hand can use
// (2 hole cards).  ge1/ge2 = per-rank count thresholds, flush2[s] =
// suit s can supply both hole cards of a flush, by_suit = suit-major
// transpose (suit_view.v).
module omaha_hole_stats (
  input  [12:0][3:0] hole,
  output reg [12:0]  ge1,
  output reg [12:0]  ge2,
  output reg [3:0]   flush2,
  output [3:0][12:0] by_suit
);
  `include "lib.vh"

  suit_view SV (.by_rank(hole), .by_suit(by_suit));

  integer r, s;
  always @* begin
    for (r = 0; r < 13; r = r + 1) begin
      ge1[r] = |hole[r];
      ge2[r] = popcnt4(hole[r]) >= 2;
    end
    for (s = 0; s < 4; s = s + 1) begin
      flush2[s] = popcnt13(by_suit[s]) >= 2;
    end
  end
endmodule

// Statistics of the board alone, capped at what a hand can use
// (3 board cards).  ge1/ge2/ge3 = per-rank count thresholds,
// flush3[s] = suit s can supply the 3 board cards of a flush,
// by_suit = suit-major transpose (suit_view.v).  A 5-card board
// cannot give two suits 3 cards each, so at most one flush3 bit is
// set: the board's lone flush-candidate suit.
module omaha_board_stats (
  input  [12:0][3:0] board,
  output reg [12:0]  ge1,
  output reg [12:0]  ge2,
  output reg [12:0]  ge3,
  output reg [3:0]   flush3,
  output [3:0][12:0] by_suit
);
  `include "lib.vh"

  suit_view SV (.by_rank(board), .by_suit(by_suit));

  integer r, s;
  always @* begin
    for (r = 0; r < 13; r = r + 1) begin
      ge1[r] = |board[r];
      ge2[r] = popcnt4(board[r]) >= 2;
      ge3[r] = popcnt4(board[r]) >= 3;
    end
    for (s = 0; s < 4; s = s + 1) begin
      flush3[s] = popcnt13(by_suit[s]) >= 3;
    end
  end
endmodule

// Combines the two per-side statistic bundles into the value word.
// Every predicate here joins a hole-side statistic with a board-side
// statistic; no card bits appear.
module omaha_value_core (
  input  [12:0]      hole_ge1,
  input  [12:0]      hole_ge2,
  input  [3:0]       hole_flush2,
  input  [3:0][12:0] hole_by_suit,
  input  [12:0]      board_ge1,
  input  [12:0]      board_ge2,
  input  [12:0]      board_ge3,
  input  [3:0]       board_flush3,
  input  [3:0][12:0] board_by_suit,
  output [23:0]      value
);
  `include "lib.vh"

  integer s, j;
  wire [12:0] present = hole_ge1 | board_ge1;   // rank present somewhere

  // ---- flush suit: needs 2 hole + 3 board of one suit.  The board
  // picks the lone candidate suit (flush3), the hole confirms it
  // (flush2); flush_hole/flush_board = that suit's rank masks
  wire [3:0] suit_flush = hole_flush2 & board_flush3;
  wire any_flush = |suit_flush;
  reg [12:0] flush_hole, flush_board;
  always @* begin
    flush_hole = 0; flush_board = 0;
    for (s = 0; s < 4; s = s + 1) begin
      if (suit_flush[s]) begin
        flush_hole = flush_hole | hole_by_suit[s];
        flush_board = flush_board | board_by_suit[s];
      end
    end
  end

  // ---- straight flush: a 5-rank window of the flush suit that admits a
  // legal split: at least 2 window ranks from the hole and at least 3
  // from the board (window_split23, lib.vh).  Hole and board flush masks
  // are disjoint per rank (a card is in one place), so the 2+3 split
  // covers all five window ranks by itself.  Wheel lowest, scored as
  // 5-high (rank index 3); highest window wins.
  reg any_sf;
  reg [3:0] sf_high;
  always @* begin
    any_sf = window_split23(wheel5(flush_hole), wheel5(flush_board));
    sf_high = any_sf ? 4'd3 : 4'd0;
    for (j = 1; j <= 9; j = j + 1)
      if (window_split23(flush_hole[j-1 +: 5], flush_board[j-1 +: 5])) begin
        any_sf = 1'b1;
        sf_high = j[3:0] + 4'd3;
      end
  end

  // ---- straight: all 5 window ranks present, and a legal split exists
  // (a rank can sit in both hole and board and fill either side, so
  // presence is a separate requirement on top of the split)
  reg any_straight;
  reg [3:0] st_high;
  always @* begin
    any_straight = (&wheel5(present))
                && window_split23(wheel5(hole_ge1), wheel5(board_ge1));
    st_high = any_straight ? 4'd3 : 4'd0;
    for (j = 1; j <= 9; j = j + 1)
      if ((&present[j-1 +: 5])
          && window_split23(hole_ge1[j-1 +: 5], board_ge1[j-1 +: 5])) begin
        any_straight = 1'b1;
        st_high = j[3:0] + 4'd3;
      end
  end

  // ---- made-rank split classes: trips_hN = trips using N hole cards,
  // pair_hN = pair using N hole cards
  wire [12:0] quad22 = hole_ge2 & board_ge2;   // quads as 2 hole + 2 board
  wire [12:0] quad13 = hole_ge1 & board_ge3;   //          1 hole + 3 board
  wire [12:0] quads = quad22 | quad13;
  wire [12:0] trips_h0 = board_ge3;
  wire [12:0] trips_h1 = hole_ge1 & board_ge2;
  wire [12:0] trips_h2 = hole_ge2 & board_ge1;
  wire [12:0] trips = trips_h0 | trips_h1 | trips_h2;
  wire [12:0] pair_h0 = board_ge2;
  wire [12:0] pair_h1 = hole_ge1 & board_ge1;
  wire [12:0] pair_h2 = hole_ge2;
  wire [12:0] pairs = pair_h0 | pair_h1 | pair_h2;

  // does a pair of class X exist strictly above / below rank r?
  // (any_above / any_below, lib.vh)
  wire [12:0] above_h0 = any_above(pair_h0);
  wire [12:0] above_h1 = any_above(pair_h1);
  wire [12:0] above_h2 = any_above(pair_h2);
  wire [12:0] below_h0 = any_below(pair_h0);
  wire [12:0] below_h1 = any_below(pair_h1);
  wire [12:0] below_h2 = any_below(pair_h2);

  // full house: trips at t (N hole cards) + a pair elsewhere using the
  // remaining 2-N hole slots; the pair can sit above or below t
  wire [12:0] full_house = (trips_h0 & (above_h2 | below_h2))
                         | (trips_h1 & (above_h1 | below_h1))
                         | (trips_h2 & (above_h0 | below_h0));

  // two pair: hole budgets must sum <= 2 and board budgets <= 3, leaving
  // the feasible class combos {h0,h1} {h0,h2} {h1,h1}.  The top rank of
  // any feasible two-pair always finds its partner strictly below (an
  // above-partner would itself top the set), so below-scans suffice.
  wire [12:0] two_pair = (pair_h0 & (below_h1 | below_h2))
                       | (pair_h1 & (below_h0 | below_h1))
                       | (pair_h2 & below_h0);

  wire any_quad     = |quads;
  wire any_fh       = |full_house;
  wire any_trips    = |trips;
  wire any_two_pair = |two_pair;
  wire any_pair     = |pairs;

  // ---- category priority ----
  wire cat_sf    = any_sf;
  wire cat_quad  = any_quad & ~any_sf;
  wire cat_fh    = any_fh & ~any_sf & ~any_quad;
  wire cat_flush = any_flush & ~any_sf & ~any_quad & ~any_fh;
  wire cat_st    = any_straight & ~any_sf & ~any_quad & ~any_fh & ~any_flush;
  wire cat_trips = any_trips & ~any_sf & ~any_quad & ~any_fh & ~any_flush
                 & ~any_straight;
  wire cat_two_pair = any_two_pair & ~any_sf & ~any_quad & ~any_fh
                    & ~any_flush & ~any_straight & ~any_trips;
  wire cat_pair = any_pair & ~any_sf & ~any_quad & ~any_fh & ~any_flush
                & ~any_straight & ~any_trips & ~any_two_pair;
  wire cat_hc = ~(any_sf | any_quad | any_fh | any_flush | any_straight
                  | any_trips | any_two_pair | any_pair);
  wire [3:0] cat = cat_sf ? 4'd9 : cat_quad ? 4'd8 : cat_fh ? 4'd7 :
                   cat_flush ? 4'd6 : cat_st ? 4'd5 : cat_trips ? 4'd4 :
                   cat_two_pair ? 4'd3 : cat_pair ? 4'd2 : 4'd1;

  // ---- primary made rank (slot n1 for quads/FH/trips/2P/1P) ----
  wire [12:0] prim_pool = cat_quad ? quads : cat_fh ? full_house :
                          cat_trips ? trips : cat_two_pair ? two_pair :
                          cat_pair ? pairs : 13'd0;
  wire [3:0] prim = top_rank13(prim_pool);
  wire [12:0] prim_bit = top_rank_bit(prim_pool);
  // the primary rank's split class.  Under one category these are one-hot:
  // two coexisting classes at one rank always imply a higher category
  // (e.g. trips_h0 & trips_h1 => 4 of the rank => quads), which priority
  // preempts.
  wire prim_t0 = trips_h0[prim], prim_t1 = trips_h1[prim],
       prim_t2 = trips_h2[prim];
  wire prim_p0 = pair_h0[prim], prim_p1 = pair_h1[prim],
       prim_p2 = pair_h2[prim];

  // ---- secondary rank: FH pair / 2P low pair (slot n2) ----
  // the class must fit the hole slots the primary left over
  wire [12:0] fh_pair_pool = (prim_t0 ? pair_h2 : 13'd0)
                           | (prim_t1 ? pair_h1 : 13'd0)
                           | (prim_t2 ? pair_h0 : 13'd0);
  wire [12:0] low_pair_pool = (prim_p0 ? (pair_h1 | pair_h2) : 13'd0)
                            | (prim_p1 ? (pair_h0 | pair_h1) : 13'd0)
                            | (prim_p2 ? pair_h0 : 13'd0);
  wire [12:0] sec_pool = (cat_fh ? fh_pair_pool :
                          cat_two_pair ? low_pair_pool : 13'd0) & ~prim_bit;
  wire [3:0] sec = top_rank13(sec_pool);
  wire [12:0] sec_bit = top_rank_bit(sec_pool);
  wire sec_p0 = pair_h0[sec], sec_p1 = pair_h1[sec], sec_p2 = pair_h2[sec];

  // ---- lone kicker: quads kicker / 2P kicker ----
  // whichever side has budget left contributes its pool; the best kicker
  // is the top of the union of the enabled pools
  wire prim_q22 = quad22[prim], prim_q13 = quad13[prim];
  wire hole_kick = (prim_p0 & sec_p1) | (prim_p1 & sec_p0);   // 1 hole slot
  wire board_kick = (prim_p1 & sec_p1) | (prim_p0 & sec_p2)   // 1 board
                  | (prim_p2 & sec_p0);                       //   slot left
  wire [12:0] kick_pool =
      (cat_quad ? ((prim_q22 ? board_ge1 : 13'd0)
                 | (prim_q13 ? hole_ge1 : 13'd0)) & ~prim_bit :
       cat_two_pair ? ((hole_kick ? hole_ge1 : 13'd0)
                     | (board_kick ? board_ge1 : 13'd0))
                     & ~prim_bit & ~sec_bit : 13'd0);
  wire [3:0] kick = top_rank13(kick_pool);

  // ---- shared kicker datapath: top-2 of the hole pool and top-3 of the
  // board pool, multiplicity-aware (top2_mult / top3_mult, lib.vh).
  // Pools are premuxed by consumer: flush -> the flush suit's masks (no
  // multiplicity), trips/1P -> rank pools minus the primary rank, high
  // card -> raw pools.
  wire [12:0] excl = (cat_trips | cat_pair) ? prim_bit : 13'd0;
  wire [12:0] hole_pool1  = cat_flush ? flush_hole : (hole_ge1 & ~excl);
  wire [12:0] hole_pool2  = cat_flush ? 13'd0 : (hole_ge2 & ~excl);
  wire [12:0] board_pool1 = cat_flush ? flush_board : (board_ge1 & ~excl);
  wire [12:0] board_pool2 = cat_flush ? 13'd0 : (board_ge2 & ~excl);
  wire [12:0] board_pool3 = cat_flush ? 13'd0 : (board_ge3 & ~excl);

  wire [7:0] hole_top2 = top2_mult(hole_pool1, hole_pool2);
  wire [3:0] hole_k0 = hole_top2[7:4];
  wire [3:0] hole_k1 = hole_top2[3:0];
  wire [11:0] board_top3 = top3_mult(board_pool1, board_pool2, board_pool3);
  wire [3:0] board_k0 = board_top3[11:8];
  wire [3:0] board_k1 = board_top3[7:4];
  wire [3:0] board_k2 = board_top3[3:0];

  // merge sorted (hole_k0 >= hole_k1) with sorted (board_k0 >= board_k1 >=
  // board_k2): Batcher odd-even, 5 compare-swaps -> top0..top4, the five
  // flush / high-card nibbles descending.  The even chain (hole_k0,
  // board_k0, board_k2) sorts to {hb_hi, ev1, ev2}, the odd chain
  // (hole_k1, board_k1) to {od0, od1}.
  wire [3:0] hb_hi = max4(hole_k0, board_k0);
  wire [3:0] hb_lo = min4(hole_k0, board_k0);
  wire [3:0] ev1 = max4(hb_lo, board_k2);
  wire [3:0] ev2 = min4(hb_lo, board_k2);
  wire [3:0] od0 = max4(hole_k1, board_k1);
  wire [3:0] od1 = min4(hole_k1, board_k1);
  wire [3:0] top0 = hb_hi;
  wire [3:0] top1 = max4(od0, ev1);
  wire [3:0] top2 = min4(od0, ev1);
  wire [3:0] top3 = max4(od1, ev2);
  wire [3:0] top4 = min4(od1, ev2);

  // trips kickers: the split fixes the leftover budget (one-hot, as above)
  wire [7:0] trip_kick = prim_t0 ? {hole_k0, hole_k1} :   // 2 hole kickers
                         prim_t1 ? {hb_hi, hb_lo} :       // 1 hole + 1 board
                                   {board_k0, board_k1};  // 2 board kickers
  // 1P kickers: 3 leftover slots by pair class
  wire [3:0] p0_k1 = max4(hb_lo, hole_k1);
  wire [3:0] p0_k2 = min4(hb_lo, hole_k1);
  wire [3:0] p1_k1 = max4(hb_lo, board_k1);
  wire [3:0] p1_k2 = min4(hb_lo, board_k1);
  wire [11:0] pair_kick =
      prim_p0 ? {hb_hi, p0_k1, p0_k2} :          // 2 hole + 1 board
      prim_p1 ? {hb_hi, p1_k1, p1_k2} :          // 1 hole + 2 board
                {board_k0, board_k1, board_k2};  // 3 board kickers
  wire [3:0] pair_k0 = pair_kick[11:8], pair_k1 = pair_kick[7:4],
             pair_k2 = pair_kick[3:0];

  // ---- output slots ----
  wire cat_fl_hc = cat_flush | cat_hc;
  wire [3:0] n1 = cat_sf ? sf_high : cat_st ? st_high :
                  cat_fl_hc ? top0 : prim;
  wire [3:0] n2 = cat_quad ? kick : cat_fl_hc ? top1 :
                  cat_trips ? trip_kick[7:4] : cat_pair ? pair_k0 :
                  (cat_fh | cat_two_pair) ? sec : 4'd0;
  wire [3:0] n3 = cat_two_pair ? kick : cat_fl_hc ? top2 :
                  cat_trips ? trip_kick[3:0] : cat_pair ? pair_k1 : 4'd0;
  wire [3:0] n4 = cat_fl_hc ? top3 : cat_pair ? pair_k2 : 4'd0;
  wire [3:0] n5 = cat_fl_hc ? top4 : 4'd0;

  assign value = {cat, n1, n2, n3, n4, n5};
endmodule
