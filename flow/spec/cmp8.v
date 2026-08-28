// 8-bit unsigned magnitude comparator: gt = (a > b), eq = (a == b).
// Sized for the 8-bit low-hand rank masks (circuit_eval.h), where
// SMALLER = stronger and 0xFF = no qualifying low, so gt means b holds
// the better low and eq covers both a genuine split and the
// nobody-qualifies case. Same contract as cmp24 at 8 bits; intended to
// be fused after two low evaluator circuits inside bitsliced equity
// kernels, while scalar code just compares the uint32_t values.
module cmp8 (
  input  [7:0] a,
  input  [7:0] b,
  output gt,
  output eq
);
  assign gt = a > b;
  assign eq = a == b;
endmodule
