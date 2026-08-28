// Teaching top: count the set bits of a 4-bit input.
//
// This is the same popcnt4 function the real evaluators use (see
// flow/spec/lib.vh) with scalar ports, so the whole spec -> synth ->
// minimize -> codegen pipeline can be walked through on a circuit small
// enough to read.
//
// Scalar ports (x0..x3, y0..y2) instead of buses so the port names survive
// verbatim into the AIG symbol table and the BLIF -- the generated C then
// talks about x0..x3, not anonymous bit indices.

module popcnt4_top (
  input  x0, x1, x2, x3,
  output y0, y1, y2
);
  `include "lib.vh"

  wire [2:0] y = popcnt4({x3, x2, x1, x0});
  assign {y2, y1, y0} = y;
endmodule
