// Suit-major view of a rank-major card set.  by_rank[r][s] holds the
// card of rank r, suit s (rank 0 = deuce .. 12 = ace); by_suit[s][r] is
// the same card, so by_suit[s] reads as one suit's 13-bit rank mask.
// Pure rewiring, no logic: the transpose costs nothing in the netlist.
module suit_view (
  input  [12:0][3:0] by_rank,
  output [3:0][12:0] by_suit
);
  genvar s, r;
  generate
    for (s = 0; s < 4; s = s + 1) begin : suit
      for (r = 0; r < 13; r = r + 1) begin : rank
        assign by_suit[s][r] = by_rank[r][s];
      end
    end
  endgenerate
endmodule
