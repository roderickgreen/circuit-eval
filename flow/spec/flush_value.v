// Flush-side evaluator, order-isomorphic 24-bit value.
//
// mask: the flushing suit's rank mask, bit 12 = ace .. bit 0 = deuce.
// value = cat<<20 | n1<<16 | n2<<12 | n3<<8 | n4<<4 | n5:
//   flush          cat=6, n1..n5 = the top five ranks of mask
//   straight flush cat=9, n1 = the straight's high rank, n2..n5 = 0
// Comparing two values as integers ranks hands exactly.

module flush_value (
  input  [12:0] mask,
  output [23:0] value
);
  `include "lib.vh"

  // straight flush: highest 5-in-a-row window of the suit, wheel lowest
  // (straight_scan, lib.vh)
  wire [4:0] sf = straight_scan(mask);
  wire any_sf = sf[4];
  wire [3:0] sf_high = sf[3:0];

  // top five ranks of the flushing suit, ace downward
  wire [19:0] top = top5_ranks(mask);

  wire [3:0] cat = any_sf ? 4'd9 : 4'd6;
  wire [3:0] n1 = any_sf ? sf_high : top[19:16];
  wire [3:0] n2 = any_sf ? 4'd0 : top[15:12];
  wire [3:0] n3 = any_sf ? 4'd0 : top[11:8];
  wire [3:0] n4 = any_sf ? 4'd0 : top[7:4];
  wire [3:0] n5 = any_sf ? 4'd0 : top[3:0];

  assign value = {cat, n1, n2, n3, n4, n5};
endmodule
