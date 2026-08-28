// Omaha 8-or-better low core, behavioral spec.
//
// hole / board: low-rank presence in hole / on board, bit 0 = ace, bit 1 =
// deuce, ... bit 7 = eight.  low: best 5-rank low as a mask (bit i = rank
// i used);
// 0xFF = no qualifying low.  For popcount-5 masks, smaller integer means
// better low, and 0xFF sorts above every valid low.
//
// A 5-rank set m is playable iff you can deal its ranks to 2 hole slots and
// 3 board slots: every rank is present somewhere, the ranks present only in
// the hole fit the 2 hole slots, and the ranks present only on the board
// fit the 3 board slots (ranks present in both can fill either side).
// The best low is the smallest playable mask, so scan m from 255 down to 0.

module low_core (
  input  [7:0] hole,
  input  [7:0] board,
  output reg [7:0] low
);
  `include "lib.vh"

  integer m;
  always @* begin
    low = 8'hFF;
    for (m = 255; m >= 0; m = m - 1)
      if (popcnt8(m[7:0]) == 5
          && ((m[7:0] & ~(hole | board)) == 8'b0)  // every rank available
          && popcnt8(m[7:0] & ~board) <= 2   // hole-only ranks fit 2 slots
          && popcnt8(m[7:0] & ~hole) <= 3)   // board-only ranks fit 3 slots
        low = m[7:0];
  end
endmodule
