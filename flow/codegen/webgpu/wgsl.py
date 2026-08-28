#!/usr/bin/env python3
"""BLIF -> WGSL compute-shader compiler for WebGPU.

Same front end as the CPU backends (logic-network BLIF from ABC; parse,
sched, gate_expr shared in ../blif.py), different back end: every wire is
one u32 / vec2<u32> / vec4<u32> per invocation, so one invocation evaluates
32/64/128 hands and the dispatch grid supplies the parallelism. Emission is
pure SSA (one `let` per gate), the same one-statement-per-wire shape as the
CPU backend; the shader compiler owns register allocation -- but not the
order it allocates in: Tint and Naga translate statements in source order
and ACO does not reschedule ALU code, so gates come out of blif.sched()
here too and the emission order reaches the GPU register allocator.

usage: wgsl.py <in.blif> [--compose <second.blif>] <out.wgsl> [--lanes 1|2|4]

--compose feeds the first circuit's outputs into the second positionally
(e.g. a value circuit composed with a value -> rank map gives cards -> rank
in one kernel). --lanes picks the word width (default 4 = vec4<u32>, 128
hands per invocation; 1 = u32 keeps the live set small for occupancy).

The generated kernel follows the C library's mask-API convention
(include/circuit_eval.h): untransposed card masks in, one u32 value per hand
out, with the mask <-> plane transposes (bspack.c's job CPU-side) run
in-shader around the circuit.  How masks map to circuit inputs is derived
from the BLIF:

  x{rank}_{suit} input names (holdem)   one mask per hand, bit 4*rank+suit;
                                        cards route through the same card ->
                                        input-plane map the C mask API uses
  52*k positional inputs (omaha)        k masks per hand, mask a covering
                                        input planes 52a..52a+51 by card id
                                        (hole planes 0..51, board 52..103)

The single generated entry point is main_eval: masks in a storage
buffer, mask-array-major: inp[a*n + i] = mask a of hand i as vec2<u32>
(.x bits 0..31, .y bits 32..51), n = arrayLength(&inp)/NMASK hands, a
multiple of the 32*lanes hands one invocation owns.  outp[i] = hand i's
value.

transpose_schedule below is the masked-swap network both in-shader
transpose directions unroll to; equity_wgsl.py imports it for the same
mask -> plane job in the equity kernels (and its --selftest verifies the
load/read convention).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from blif import parse, sched, gate_expr, card_planes


def emit_stage(path, input_ref, stmts, nidx, prefix="t"):
    """Compile one BLIF into (name, expr) statements appended to stmts;
    input_ref maps positional input index -> WGSL expr. Returns the WGSL
    exprs for the stage's outputs (positional), the input name list, the
    output count, and the updated statement counter."""
    _, inputs, outputs, gates = parse(path)
    order = sched(inputs, outputs, gates)
    inpos = {w: i for i, w in enumerate(inputs)}
    local = {}

    def ref(w):
        if w in local:
            return local[w]
        return input_ref(inpos[w])

    for n in order:
        e = gate_expr(*gates[n], ref=ref)
        name = f"{prefix}{nidx}"
        nidx += 1
        stmts.append((name, e))
        local[n] = name
    return [ref(o) for o in outputs], inputs, len(outputs), nidx


def transpose_schedule():
    """(j, m, k) triples of the in-place 32x32 masked-swap transpose
    network (Hacker's Delight 7-3), in emission order.  Convention: load
    the 32 hand words reversed (var 31-t = hand t), run the network, read
    reversed (plane p = var 31-p, bit t = hand t's bit p).  Verified by
    equity_wgsl.py --selftest."""
    schedule = []
    j, m = 16, 0x0000FFFF
    while j:
        for k in range(32):
            if not (k & j):
                schedule.append((j, m, k))
        j >>= 1
        if j:
            m = (m ^ (m << j)) & 0xFFFFFFFF
    return schedule


def xpose_stmts(v, tag):
    """The unrolled network over vars {v}0..{v}31, one `word` each: every
    u32 lane of the word transposes independently (masks and shift counts
    broadcast, so the same statements serve every --lanes width).  All
    indices are constants on purpose -- the vars stay in registers.  tag
    keeps the let temporaries of repeated instantiations distinct."""
    lines = []
    for j, m, k in transpose_schedule():
        t = f"s{tag}_{j}_{k}"
        lines += [
            f"  let {t} = ({v}{k} ^ ({v}{k + j} >> word({j}u))) & word(0x{m:08x}u);",
            f"  {v}{k} = {v}{k} ^ {t};",
            f"  {v}{k + j} = {v}{k + j} ^ ({t} << word({j}u));",
        ]
    return lines


def main():
    args = sys.argv[1:]
    LANES, COMPOSE = 4, None
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--lanes":
            LANES = int(args[i + 1]); i += 2
        elif args[i] == "--compose":
            COMPOSE = args[i + 1]; i += 2
        else:
            pos.append(args[i]); i += 1
    blif_path, out_path = pos

    stmts = []
    out_refs, inputs, nout, nidx = emit_stage(
        blif_path, lambda i: f"(*x)[{i}]", stmts, 0)
    nin = len(inputs)
    if COMPOSE:
        mid, nout1 = out_refs, nout
        out_refs, inputs2, nout, nidx = emit_stage(
            COMPOSE, lambda i, m=mid: m[i], stmts, nidx)
        if len(inputs2) != nout1:
            sys.exit(f"compose mismatch: {nout1} outputs -> {len(inputs2)} inputs")

    if nout > 32:
        sys.exit(f"{nout} outputs do not fit the one-u32-per-hand output layout")

    # mask layout, from the first stage's inputs (see module docstring)
    cardmap = card_planes(inputs)
    if cardmap is not None and all(p is not None for p in cardmap):
        nmask = 1
    elif nin % 52 == 0:
        cardmap = None
        nmask = nin // 52
    else:
        sys.exit(f"no mask convention for {nin} inputs: need x{{rank}}_{{suit}} "
                 "names or a multiple of 52 positional planes")

    WORD = {1: "u32", 2: "vec2<u32>", 4: "vec4<u32>"}[LANES]
    HANDS = 32 * LANES

    # ---- main_eval body: transpose in, circuit, transpose out ----
    ev = ["@compute @workgroup_size(64)",
          "fn main_eval(@builtin(global_invocation_id) g: vec3<u32>) {",
          "  let n = arrayLength(&inp) / NMASK;",
          f"  let base = g.x * {HANDS}u;",
          "  if (base >= n) { return; }",
          "  var x: array<word, NIN>;"]
    for a in range(nmask):
        d = "var " if a == 0 else ""
        off = "base" if a == 0 else f"{a}u * n + base"
        ev += [f"  let o{a} = {off};",
               f"  // mask array {a}: gather {HANDS} hands, 32 per u32 lane,",
               "  // loaded reversed for the transpose network"]
        for t in range(32):
            es = []
            for w in range(LANES):
                e = f"e{a}_{w}_{t}"
                ev.append(f"  let {e} = inp[o{a} + {32 * w + t}u];")
                es.append(e)
            ev.append(f"  {d}rl{31 - t} = word({', '.join(x + '.x' for x in es)});")
            ev.append(f"  {d}rh{31 - t} = word({', '.join(x + '.y' for x in es)});")
        ev += xpose_stmts("rl", f"l{a}")
        ev += xpose_stmts("rh", f"h{a}")
        ev.append("  // planes -> circuit inputs ("
                  + ("the card -> input map" if cardmap else "positional")
                  + "); planes of unused mask bits are dead code")
        for c in range(52):
            p = f"rl{31 - c}" if c < 32 else f"rh{31 - (c - 32)}"
            ev.append(f"  x[{cardmap[c] if cardmap else 52 * a + c}] = {p};")
    ev += ["  var y: array<word, NOUT>;",
           "  circuit(&x, &y);",
           "  // value planes -> per-hand u32 values: the same network back"]
    for k in range(32):
        ev.append(f"  rl{31 - k} = {f'y[{k}]' if k < nout else 'ZERO'};")
    ev += xpose_stmts("rl", "v")
    for t in range(32):
        for w in range(LANES):
            c = "" if LANES == 1 else "." + "xyzw"[w]
            ev.append(f"  outp[base + {32 * w + t}u] = rl{31 - t}{c};")
    ev.append("}")

    with open(out_path, "w") as f:
        W = f.write
        W(f"// generated by wgsl.py -- do not edit\n")
        W(f"// meta: nin={nin} nout={nout} nmask={nmask} lanes={LANES}\n")
        W(f"alias word = {WORD};\n")
        W(f"const NIN = {nin}u;\nconst NOUT = {nout}u;\nconst NMASK = {nmask}u;\n")
        W("const ZERO = word();\nconst ONES = ~word();\n\n")
        W("// The C mask API's convention (include/circuit_eval.h): untransposed\n"
          "// card masks in, one value per hand out.  inp[a*n + i] = mask a of\n"
          "// hand i, bit 4*rank+suit (.x bits 0..31, .y bits 32..51), with\n"
          "// n = arrayLength(&inp) / NMASK a multiple of the hands one\n"
          "// invocation owns (32 * lanes).  Card space, not plane space: cards\n"
          "// route to circuit input planes in-shader, the job bs_transpose_map\n"
          "// does CPU-side (unlike the equity kernels, whose host pre-maps\n"
          "// work-list masks into plane space).\n")
        W("@group(0) @binding(0) var<storage, read> inp : array<vec2<u32>>;\n")
        W("// one u32 value per hand (every value encoding here is <= 24 bits)\n")
        W("@group(0) @binding(1) var<storage, read_write> outp : array<u32>;\n\n")
        W("fn circuit(x: ptr<function, array<word, NIN>>,\n"
          "           y: ptr<function, array<word, NOUT>>) {\n")
        for name, e in stmts:
            W(f"  let {name} = {e};\n")
        for k, r in enumerate(out_refs):
            W(f"  (*y)[{k}] = {r};\n")
        W("}\n\n")
        W("\n".join(ev) + "\n")

    print(f"stmts={len(stmts)} nin={nin} nout={nout} nmask={nmask} "
          f"lanes={LANES} hands/invocation={HANDS} -> {out_path}")


if __name__ == "__main__":
    main()
