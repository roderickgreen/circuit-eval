# popcnt4: the whole pipeline on a circuit you can read

The poker evaluators in this repo are built by one process: write a
behavioral Verilog spec, synthesize it to an AIG with yosys, minimize it
to 2-input gates with ABC, then compile the gate network to bitsliced C.
The real circuits are 1,000+ gates; this directory runs the identical
process on a 4-bit population count (9 gates), so every intermediate
artifact fits on a screen.

This uses the yosys and abc submodules; build them first (see "Building
the toolchain" in the repo root `README.md`).

```
make        # builds everything and runs the exhaustive test
```

## Stage 1 -- behavioral spec to AIG (yosys)

`popcnt4_top.v` wraps the same `popcnt4` function the real evaluators use
(`flow/spec/lib.vh`): a plain Verilog for-loop that adds up four bits.
yosys elaborates it and lowers everything to an **AIG** (`popcnt4.aig`) --
an and-inverter graph, i.e. nothing but 2-input ANDs and inverters. That
is deliberately dumb output: yosys only *translates*, all optimization is
left to ABC, which is better at it.

## Stage 2 -- minimize and map (ABC)

ABC restructures the AIG (`dch`, don't-care-aware resynthesis) and maps it
onto 2-input LUTs (`if -K 2`), producing `popcnt4.blif`. The BLIF is worth
reading: each `.names a b y` block is one gate, and the lines under it are
its truth table (`10 1` means "a=1, b=0 gives y=1"). For popcnt4 ABC finds
9 gates in 4 levels -- which turn out to be the textbook adder: a 3-XOR
parity chain plus carry logic.

## Stage 3 -- gates to bitsliced C (bitslice64.py)

`flow/codegen/cpu/bitslice64.py` turns the BLIF into straight-line C
(`popcnt4.c`): one `uint64_t` per wire, one bitwise statement per gate, in
topological order.

The trick is **bitslicing**: instead of evaluating the circuit once per
input, give every wire a 64-bit word whose bit *i* belongs to evaluation
*i*. A 2-input gate then costs one machine instruction *for 64 evaluations
at once* -- there is no branching, no lookup tables, and the "parallelism"
is just ordinary bitwise AND/OR/NOT.

(The production backend, `flow/codegen/cpu/bitslice.py`, is the same code
shape at the target's vector width -- 128, 256, or 512 lanes per statement
instead of 64, and that is the main difference. Read this one first.)

## Stage 4 -- pack, evaluate, unpack (pack.c, main.c)

The kernel's calling convention is planes, not values: input word *b*
carries bit *b* of all 64 evaluations. `pack.c` is the readable bridge
between that layout and ordinary arrays -- `popcnt4_pack` (values to
planes), `popcnt4_unpack` (planes to values), and `popcnt4_batch`, which
bundles pack + eval + unpack into one call. Each direction is just a
bit-matrix transpose written as the obvious double loop.

The production API (`include/bsapi.h` /
`lib/bspack/`) has the identical three-function shape --
`bs_transpose`, `bs_untranspose`, `bs_eval_hands` -- but at 256 hands x 52
bits per 3 ns evaluation the per-bit loop would cost more than the
evaluator, so there the transpose runs as six stages of masked swaps.
Every line of bspack.c is an optimization of the loops in pack.c.

popcnt4 has 16 possible inputs and one call evaluates 64 lanes, so the
driver (`main.c`) covers the entire truth table four times in a single
call by driving lane *i* with x = *i* mod 16. Packing those values makes
the four input planes the familiar truth-table constants 0xAAAA...,
0xCCCC..., 0xF0F0..., 0xFF00... (the driver prints them). It then prints
the recovered truth table and checks all 64 lanes against
`__builtin_popcount`.

## The same gates on a GPU (popcnt4.wgsl)

`popcnt4.wgsl` is the nine gates again as a WebGPU compute shader: one
`u32` per wire (32 lanes instead of 64) and a dispatch in place of the
call. Diff it against `popcnt4.c`; the gate body is a transliteration
and its header comment covers the rest. It takes planes directly, so
there is no pack stage; the production kernels
(`flow/codegen/webgpu/wgsl.py`) also run the mask <-> plane transpose
in-shader, which is most of their length.
