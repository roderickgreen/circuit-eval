#!/usr/bin/env python3
"""BLIF -> straight-line bitsliced C over BS_BATCH-lane vector words.

Reads a logic-network BLIF (as written by ABC's write_blif: nodes are small
SOPs, mostly 2-input), topologically orders the gates, and emits one C
statement per gate: every wire is a local `bs_word` (GCC vector
extensions).  The word width is a compile-time parameter of the OUTPUT,
not of this generator: the file includes include/bsword.h, which carries the
BS_BATCH probe (512 lanes where the compile target has AVX-512, 128 on
NEON, else 256, overridable with -DBS_BATCH), so one generated file
serves every width and can never drift from its siblings; compiling it
needs include/ on the include path.  bitslice64.py is
the same backend at teaching width (one uint64_t per wire). The BLIF
front end (parse, topo, gate_expr) is shared across backends in
../blif.py.

Three things are decided here rather than left to the C compiler.

The gate cover.  The netlist is re-covered in gates of at most three inputs
(blif.lut3_cover: a gate feeding exactly one consumer is absorbed into it
when their inputs together are three wires or fewer), and on AVX-512 every
3-input gate, and every 2-input gate that is not a plain AND/ANDN/OR/XOR,
is emitted as an explicit vpternlog intrinsic through the T3 macro in
include/bsword.h.  Elsewhere T3 expands to the gate's boolean expression.
Left to the compiler, the cover is whatever its combiner rebuilds from
the one-gate-per-statement form, and that varies by compiler version:
one that also folds a gate with several consumers into each of them as a
ternary-logic instruction recomputes the gate per consumer, keeps its
inputs live across all of them, and pays a register copy per destructive
instruction -- a third more instructions and bytes on the omaha circuit.
The size matters because the circuit runs from the op cache: straight-line
code past roughly 600 64-byte windows on a Zen 5 core falls back to the
x86 decoders at a fraction of the speed.  Pinning the cover makes the
instruction count a property of the generated file, not of the compiler.
The operand order of each T3 puts an operand whose last use is this gate
first: vpternlog overwrites its first operand, so that is the one the
allocator can reuse without a copy.

The inputs.  Up to a hundred of the circuit's inputs are live at once
deep into the schedule, and when the allocator spills one it can either
store it to the frame and reload it from there, or re-read it from the
input array where it already sits.  gcc takes the second only while the
pointer the value was loaded through is still live, and with all inputs
loaded at the top that pointer would die right there; so BS_KEEP(in)
(bsword.h), an empty asm that reads the pointer, is emitted just before
the output stores.  Every spilled input is then a re-read of in[k], no
spill store, and the frame shrinks by the inputs' share.

The gate order.  The gates come out of blif.sched(), not blif.topo().
One local per gate means the order emitted here is the order a register
allocator starts from, and a plain topological order leaves values live
far from their consumers; see sched()'s docstring.  A compiler may
reorder it again before allocating -- gcc does with
-flive-range-shrinkage, which lib/Makefile passes -- but it schedules
from what is written here, and the two orders do not converge.

Nothing else: gcc does its own liveness, register allocation and
scheduling from the one-local-per-gate form, so recycling storage slots
by liveness buys nothing.  The speed over bitslice64.py is the word width.

Interface (consumed by verify/pack/bsglue.c and bench_pack.c; lib/
links the --sym form through lib/circuit_eval.c):
  void bs_eval(const bs_word *in, bs_word *out)  -- in/out in BLIF port order
  const int bs_nb = 1                      -- batches consumed per call
  const int bs_num_outputs
  const int bs_card_input[52]              -- input index of card 4*rank+suit,
                                              when inputs are named x{r}_{s};
  const int bs_num_inputs                  -- exported instead otherwise

usage: bitslice.py [--sym NAME] <in.blif> <out.c>
  --sym NAME renames the whole export set: the evaluator becomes NAME() and
  the metadata NAME_nb / NAME_num_outputs / NAME_card_input /
  NAME_num_inputs -- for circuits checked into lib/generated/ under their
  published names (lib/Makefile regen), where the default bs_* names would collide.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from blif import (parse, sched, gate_expr, card_planes, lut3_cover,
                  ternlog_imm, needs_ternlog)

args = sys.argv[1:]
fn, meta = "bs_eval", "bs_"
if args and args[0] == "--sym":
    fn, meta = args[1], args[1] + "_"
    args = args[2:]

if len(args) != 2:
    sys.exit("usage: bitslice.py [--sym NAME] <in.blif> <out.c>")
blif_path, out_path = args

_, inputs, outputs, gates = parse(blif_path)
gates3, truth, absorbed = lut3_cover(inputs, outputs, gates)
order = sched(inputs, outputs, gates3)
in_set = set(inputs)

# inputs become i0, i1, ...; internal wires t0, t1, ... in emit order
cname = {w: f"i{k}" for k, w in enumerate(inputs)}
for n in order:
    cname[n] = f"t{len(cname) - len(inputs)}"

# last use of each wire, in emit order, for the T3 operand order
last = {}
for i, g in enumerate(order):
    for d in gates3[g][0]:
        last[d] = i
for w in outputs:
    last[w] = len(order)


def expr(g):
    """The gate as a boolean expression: the original gates, any absorbed
    one substituted (parenthesized) where its wire was read.  The same
    dataflow as the uncovered form, for targets without ternary logic."""
    ref = lambda w: f"({expr(w)})" if w in absorbed else cname[w]
    return gate_expr(*gates[g], ref=ref)


stmts = []
for i, g in enumerate(order):
    ins, tt = gates3[g][0], truth[g]
    e = expr(g)
    if needs_ternlog(ins, tt):
        ops = list(ins)
        dying = [w for w in ops if w not in in_set and last[w] == i]
        if dying:
            ops.remove(dying[0])
            ops.insert(0, dying[0])
        while len(ops) < 3:
            ops.append(ops[-1])
        imm = ternlog_imm(ops, ins, tt)
        e = (f"T3(0x{imm:02x}, {cname[ops[0]]}, {cname[ops[1]]}, "
             f"{cname[ops[2]]}, {e})")
    stmts.append((cname[g], e))

card_pos = card_planes(inputs)

# ---- emit ----
with open(out_path, "w") as f:
    W = f.write
    W(f"/* generated by bitslice.py from {blif_path} -- do not edit */\n")
    W("#include \"bsword.h\"   /* BS_BATCH, bs_word, T3 */\n")
    used = {e for _, e in stmts}
    if any("ZERO" in e for e in used):
        W("static const bs_word ZERO = {0};   /* zero-fills at any width */\n")
    if any("ONES" in e for e in used):
        W("#define ONES (~(bs_word){0})\n")
    W(f"\nvoid {fn}(const bs_word *in, bs_word *out) {{\n")
    for k, w in enumerate(inputs):
        W(f"    bs_word {cname[w]} = in[{k}];\n")
    for name, expr in stmts:
        W(f"    bs_word {name} = {expr};\n")
    W("    BS_KEEP(in);\n")
    for k, w in enumerate(outputs):
        W(f"    out[{k}] = {cname[w]};  /* {w} */\n")
    W("}\n")
    W(f"const int {meta}nb = 1;\n")
    W(f"const int {meta}num_outputs = {len(outputs)};\n")
    if card_pos is not None:
        W(f"const int {meta}card_input[52] = {{"
          + ",".join(map(str, card_pos)) + "};\n")
    else:
        W(f"const int {meta}num_inputs = {len(inputs)};\n")

print(f"{out_path}: {len(inputs)} inputs, {len(outputs)} outputs, "
      f"{len(order)} gates ({len(absorbed)} absorbed, "
      f"{sum(1 for _, e in stmts if e.startswith('T3('))} ternary)")
