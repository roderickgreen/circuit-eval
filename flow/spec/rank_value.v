// Rank-side evaluator, order-isomorphic 24-bit value.
//
// counts[r] = how many cards of rank r are held, 0..4 (rank 0 = deuce,
// rank 12 = ace).
// value = cat<<20 | n1<<16 | n2<<12 | n3<<8 | n4<<4 | n5,
// cat: HC=1 1P=2 2P=3 3K=4 ST=5 FH=7 Q=8 (6 and 9 are the flush side's),
// n1..n5 = the made-hand tuple ranks, most significant first, zero-padded:
//   HC: a b c d e   1P: p k1 k2 k3   2P: hi lo k   3K: t k1 k2
//   ST: high        FH: t p          Q:  q k
// Comparing two values as integers ranks hands exactly.

module rank_value (
  input  [12:0][2:0] counts,
  output [23:0] value
);
  `include "lib.vh"

  // per-rank shape indicators
  reg [12:0] pres, sing, pair, trip, quad, ge2;
  integer r;
  always @* begin
    for (r = 0; r < 13; r = r + 1) begin
      pres[r] = |counts[r];               // >= 1 card of this rank
      sing[r] = counts[r] == 3'd1;
      pair[r] = counts[r] == 3'd2;
      trip[r] = counts[r] == 3'd3;
      quad[r] = counts[r][2];             // count bit 2 <=> 4 cards
      ge2[r]  = counts[r][1] | counts[r][2];  // >= 2 cards
    end
  end

  wire [3:0] n_pair = popcnt13(pair);
  wire [3:0] n_trip = popcnt13(trip);
  wire any_quad = |quad;

  // straights: highest 5-in-a-row window of rank presence, wheel lowest
  // (straight_scan, lib.vh)
  wire [4:0] st = straight_scan(pres);
  wire any_straight = st[4];
  wire [3:0] st_high = st[3:0];

  // category, in priority order: quads > full house > straight > trips >
  // two pair > pair > high card (a full house is two trips, or trips+pair;
  // "pair" means count exactly 2 throughout)
  wire fh = (n_trip >= 2) | ((n_trip >= 1) & (n_pair >= 1));
  wire cat_quad = any_quad;
  wire cat_fh = fh & ~any_quad;
  wire cat_st = any_straight & ~any_quad & ~fh;
  wire cat_trips = (n_trip >= 1) & ~any_quad & ~fh & ~any_straight;
  wire cat_two_pair = (n_pair >= 2) & (n_trip == 0) & ~any_quad
                    & ~any_straight;
  wire cat_pair = (n_pair == 1) & (n_trip == 0) & ~any_quad & ~any_straight;
  wire cat_hc = (n_pair == 0) & (n_trip == 0) & ~any_quad & ~any_straight;
  wire [3:0] cat = cat_quad ? 4'd8 : cat_fh ? 4'd7 : cat_st ? 4'd5 :
                   cat_trips ? 4'd4 : cat_two_pair ? 4'd3 :
                   cat_pair ? 4'd2 : 4'd1;

  // tuple selections -- every one is "the best rank with some property,
  // excluding ranks already spoken for" (top_rank13 / top_rank_bit /
  // top5_ranks, lib.vh)
  wire [19:0] singles = top5_ranks(sing);  // five highest single ranks
  wire [3:0] single0 = singles[19:16];     // (kickers)
  wire [3:0] single1 = singles[15:12];
  wire [3:0] single2 = singles[11:8];
  wire [3:0] single3 = singles[7:4];
  wire [3:0] single4 = singles[3:0];
  wire [12:0] pair_hi_bit = top_rank_bit(pair);
  wire [12:0] pair_lo_bit = top_rank_bit(pair & ~pair_hi_bit);
  wire [3:0] pair_hi = top_rank13(pair);
  wire [3:0] pair_lo = top_rank13(pair & ~pair_hi_bit);
  wire [3:0] trip_rank = top_rank13(trip);
  wire [3:0] quad_rank = top_rank13(quad);
  // quads kicker: best other rank held
  wire [3:0] quad_kick = top_rank13(pres & ~quad);
  // full-house pair: best 2+ rank other than the trips rank
  wire [3:0] fh_pair = top_rank13(ge2 & ~top_rank_bit(trip));
  // two-pair kicker: best rank held outside the two pairs
  wire [3:0] tp_kick = top_rank13(pres & ~pair_hi_bit & ~pair_lo_bit);

  // nibble slots by category
  wire [3:0] n1 = cat_quad ? quad_rank : cat_fh ? trip_rank :
                  cat_st ? st_high : cat_trips ? trip_rank :
                  (cat_two_pair | cat_pair) ? pair_hi : single0;
  wire [3:0] n2 = cat_quad ? quad_kick : cat_fh ? fh_pair : cat_st ? 4'd0 :
                  cat_trips ? single0 : cat_two_pair ? pair_lo :
                  cat_pair ? single0 : single1;
  wire [3:0] n3 = (cat_quad | cat_fh | cat_st) ? 4'd0 :
                  cat_trips ? single1 : cat_two_pair ? tp_kick :
                  cat_pair ? single1 : single2;
  wire [3:0] n4 = cat_pair ? single2 : cat_hc ? single3 : 4'd0;
  wire [3:0] n5 = cat_hc ? single4 : 4'd0;

  assign value = {cat, n1, n2, n3, n4, n5};
endmodule
