// 24-bit unsigned magnitude comparator: gt = (a > b), eq = (a == b).
// Sized for the 24-bit order-isomorphic hand values (circuit_eval.h),
// where bigger = stronger and equal = tie, so gt/eq classify a showdown
// as win/tie/loss directly. Intended to be fused after two evaluator
// circuits inside bitsliced equity kernels; in scalar code a plain
// integer compare already does this.
module cmp24 (
  input  [23:0] a,
  input  [23:0] b,
  output gt,
  output eq
);
  assign gt = a > b;
  assign eq = a == b;
endmodule
