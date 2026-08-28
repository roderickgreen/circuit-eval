#!/usr/bin/env python3
"""Exact-enumeration equity kernel generator for WebGPU (deal-major).

Wraps the circuit bodies (via wgsl.py's BLIF front end) in a
`main_equity` entry point that walks host-built work lists:

  invocation index -> one villain draw (one entry of the villain list) + 32
  consecutive entries of the board list -> bit-transpose the 32 board masks
  into per-lane input planes -> hero eval + villain eval -> bitslice compare
  of the order-isomorphic values -> popcount into an atomic tally.

The kernel contains no combinatorics.  Both lists are enumerated on the CPU
(demo/src/lib/worklist.ts) as packed card-mask pairs in plane space (bit index =
circuit input plane, not card id -- the host maps cards through planeOfCard
below, so the kernel never needs the card<->plane permutation).  This is
the same shape as the C library's mask API: masks in, and the
shader's first step is the job bspack.c does there -- a 32x32 bit transpose
from per-hand masks to per-plane words (Hacker's Delight 7-3, unrolled so
every private-array index is a constant and everything stays in registers).

The two lists are enumerated independently over the live deck, so a villain
draw and a board can claim the same card; such pairs are not deals and are
dropped in-kernel by one mask test per lane.  Exactly C(nlive - v, k)
boards survive each villain draw, so the "boards" tally still counts every
real deal once and the host's combinatorial cross-check is unchanged.

Knowns arrive as uniforms: hero/villain hole masks and the revealed-board
mask, all in plane space; the lists carry only the drawn (unknown) cards.
The lists are walked in the order the host built them -- sampling order,
if any, is the host's business.

A partly unknown hero is a third axis, and it lives on the uniform: the
host enumerates hero draws itself, rewriting the hero mask (known |
drawn) and the hdraw words (the drawn part alone) between dispatches.
The lists are built once, without knowledge of any particular hero draw,
so entries can claim a drawn hero card; hdraw feeds the same collision
test as the villain draw and such entries are dropped in-kernel.  Every
hero draw then counts exactly C(nlive - h, v) * C(nlive - h - v, k)
deals, and the host's combinatorial cross-check extends per axis.

usage: equity_wgsl.py --style holdem|omaha --high <high.blif>
                      [--low <low.blif>] <out.wgsl>
       equity_wgsl.py --selftest        (verify the transpose network)

  holdem  52-input circuit, hole and board share the presence planes;
          plane order is derived from the x{rank}_{suit} input names.
  omaha   104-input circuits (hole planes 0..51 by card id, board planes
          52..103); --low adds the 8-or-better low circuit (same 104
          inputs) and switches the tally to quarter-pot units.

Tally layout (u64 pairs): high-only [win, tie, boards]; hi-lo [hero
quarters, boards, high win, high tie, hero low, split low, no low, hero
scoops] -- the per-half counters feed the UI's breakdown and cost six
popcount+tadd on masks the quarter-pot tail already computes.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from blif import parse, sched, gate_expr, card_planes
from wgsl import transpose_schedule

MASK32 = 0xFFFFFFFF


def emit_circuit_fn(path, fn_name, prefix):
    """One BLIF -> a standalone WGSL fn evaluating the whole circuit.

    Gates come out of blif.sched(), not blif.topo(): Tint and Naga
    translate statements in source order and ACO does not reschedule ALU
    code, so the emission order reaches the GPU register allocator too."""
    _, inputs, outputs, gates = parse(path)
    order = sched(inputs, outputs, gates)
    inpos = {w: i for i, w in enumerate(inputs)}
    local = {}

    def ref(w):
        if w in local:
            return local[w]
        return f"(*x)[{inpos[w]}]"

    lines = [f"fn {fn_name}(x: ptr<function, array<word, {len(inputs)}>>,\n"
             f"    y: ptr<function, array<word, {len(outputs)}>>) {{"]
    for i, n in enumerate(order):
        name = f"{prefix}{i}"
        lines.append(f"  let {name} = {gate_expr(*gates[n], ref=ref)};")
        local[n] = name
    for k, o in enumerate(outputs):
        lines.append(f"  (*y)[{k}] = {ref(o)};")
    lines.append("}")
    return "\n".join(lines), inputs, len(outputs)


def plane_of_card(style, inputs):
    """card id (4*rank+suit) -> input plane index, for the meta line."""
    if style == "omaha":
        return list(range(52))  # hole plane == card id; board = 52 + id
    m = card_planes(inputs)
    assert m is not None and all(p is not None for p in m), \
        "holdem inputs must be x{r}_{s}"
    return m


def cmp_fn(name, n):
    """MSB-first bitslice compare: returns (a>b mask, a==b mask)."""
    return f"""fn {name}(a: ptr<function, array<word, {n}>>,
    b: ptr<function, array<word, {n}>>) -> array<word, 2> {{
  var gt = ZERO;
  var eq = ONES;
  for (var i = 0u; i < {n}u; i++) {{
    let k = {n - 1}u - i;
    gt |= eq & (*a)[k] & ~(*b)[k];
    eq &= ~((*a)[k] ^ (*b)[k]);
  }}
  return array<word, 2>(gt, eq);
}}"""


def transpose_stmts(pfx, ind="    "):
    """Unrolled transpose on named vars {pfx}0..{pfx}31.  With input loaded
    reversed ({pfx}{31-t} = lane t) the output reads reversed: plane p =
    {pfx}{31-p} bit t = lane t's bit p.  Verified by --selftest."""
    lines = []
    for j, m, k in transpose_schedule():
        t = f"t{pfx}{j}_{k}"
        lines += [
            f"{ind}let {t} = ({pfx}{k} ^ ({pfx}{k + j} >> {j}u)) & 0x{m:08x}u;",
            f"{ind}{pfx}{k} = {pfx}{k} ^ {t};",
            f"{ind}{pfx}{k + j} = {pfx}{k + j} ^ ({t} << {j}u);",
        ]
    return lines


def lane_load_stmts(buf, count, base, clash_lo, clash_hi, ind):
    """32 consecutive list entries -> per-lane mask vars a0..a31 / b0..b31,
    loaded reversed for the transpose.  An entry sharing a card with
    (clash_lo, clash_hi) -- the other half of this deal -- is not a deal
    and leaves `valid`.  Lanes past the end of the list reread the last
    entry; their valid bits are already clear."""
    lines = [ind + " ".join(f"var a{i} = 0u;" for i in range(32)),
             ind + " ".join(f"var b{i} = 0u;" for i in range(32))]
    for t in range(32):
        lines += [
            f"{ind}let e{t} = {buf}[min({base} + {t}u, {count} - 1u)];",
            f"{ind}if ((({clash_lo} & e{t}.x) | ({clash_hi} & e{t}.y)) != 0u)"
            f" {{ valid = valid & ~(1u << {t}u); }}",
            f"{ind}a{31 - t} = e{t}.x;",
            f"{ind}b{31 - t} = e{t}.y;",
        ]
    return lines


def selftest():
    import random
    rnd = random.Random(1)
    masks = [rnd.getrandbits(32) for _ in range(32)]
    a = [masks[31 - i] for i in range(32)]        # load reversed
    for j, m, k in transpose_schedule():
        t = (a[k] ^ (a[k + j] >> j)) & m
        a[k] ^= t
        a[k + j] = (a[k + j] ^ (t << j)) & MASK32
    for p in range(32):
        w = a[31 - p]                             # read reversed
        for t in range(32):
            assert (w >> t) & 1 == (masks[t] >> p) & 1, (p, t)
    print("transpose selftest: ok")


def main():
    args = sys.argv[1:]
    if args == ["--selftest"]:
        selftest()
        return
    style = high = low = None
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--style":
            style = args[i + 1]; i += 2
        elif args[i] == "--high":
            high = args[i + 1]; i += 2
        elif args[i] == "--low":
            low = args[i + 1]; i += 2
        else:
            pos.append(args[i]); i += 1
    (out_path,) = pos
    assert style in ("holdem", "omaha") and high
    assert low is None or style == "omaha", "--low is an omaha option"
    hilo = low is not None

    high_fn, high_inputs, high_nout = emit_circuit_fn(high, "high_circuit", "h")
    assert high_nout == 24
    nin = len(high_inputs)
    assert nin == (52 if style == "holdem" else 104)
    parts = [high_fn, cmp_fn("cmp24", 24)]
    if hilo:
        low_fn, low_inputs, low_nout = emit_circuit_fn(low, "low_circuit", "l")
        assert len(low_inputs) == 104 and low_nout == 8
        parts += [low_fn, cmp_fn("cmp8", 8)]

    pc = plane_of_card(style, high_inputs)

    header = f"""// generated by equity_wgsl.py -- do not edit
// meta: equity style={style} nin={nin} hilo={int(hilo)}
// planeOfCard: {",".join(map(str, pc))}
alias word = u32;
const ZERO = 0u;
const ONES = 0xffffffffu;

struct Params {{
  hero_lo: u32,    // hero hole mask, plane space
  hero_hi: u32,
  vill_lo: u32,    // known villain holes; the drawn part is per list entry
  vill_hi: u32,
  board_lo: u32,   // revealed board mask, plane space
  board_hi: u32,
  nboards: u32,    // board list length
  nvillains: u32,  // villain list length
  base: u32,       // first invocation index of this dispatch (chunking)
  hdraw_lo: u32,   // drawn (unknown) hero cards this dispatch -- the host
  hdraw_hi: u32,   //   enumerates hero draws by rewriting the uniform; a
                   //   subset of the hero mask, used only to drop list
                   //   entries that claim one of these cards
}}
@group(0) @binding(0) var<uniform> P: Params;
// work lists, built host-side (demo/src/lib/worklist.ts): one entry per
// combination of drawn cards, as a plane-space mask pair (.x holds planes
// 0..31, .y planes 32..51)
@group(0) @binding(1) var<storage, read> boards: array<vec2<u32>>;
@group(0) @binding(2) var<storage, read> villains: array<vec2<u32>>;
// counter i is the u64 pair (tally[2i], tally[2i+1]) -- big spots overflow
// a single u32 (matches the board-major kernels)
@group(0) @binding(3) var<storage, read_write> tally: array<atomic<u32>>;

fn tadd(i: u32, x: u32) {{
  let old = atomicAdd(&tally[2u * i], x);
  if (old > ONES - x) {{ atomicAdd(&tally[2u * i + 1u], 1u); }}
}}

fn maskbit(lo: u32, hi: u32, p: u32) -> word {{
  var w = lo;
  if (p >= 32u) {{ w = hi; }}
  if (((w >> (p & 31u)) & 1u) == 1u) {{ return ONES; }}
  return ZERO;
}}"""

    # --- the work-list prologue, shared by both styles ---
    prologue_lines = ["""@compute @workgroup_size(64)
fn main_equity(@builtin(global_invocation_id) g: vec3<u32>) {
  let invPerV = (P.nboards + 31u) / 32u;
  let ninv = P.nvillains * invPerV;
  let gidx = P.base + g.x;
  if (gidx >= ninv) { return; }
  let vIdx = gidx / invPerV;
  let bbase = (gidx % invPerV) * 32u;
  let nvalid = min(32u, P.nboards - bbase);
  var valid = ONES;
  if (nvalid < 32u) { valid = (1u << nvalid) - 1u; }
  // this invocation's villain draw, constant across its 32 lanes
  let vm = villains[vIdx];
  // a villain draw claiming a drawn hero card is not part of any deal
  if (((vm.x & P.hdraw_lo) | (vm.y & P.hdraw_hi)) != 0u) { return; }
  // 32 board-list entries as per-lane masks, loaded reversed for the
  // transpose.  A board sharing a card with the villain draw or the drawn
  // hero cards is not a deal: its lane leaves `valid`.  Lanes past the end
  // of the list reread the last entry; their valid bits are already clear.
  let cl_lo = vm.x | P.hdraw_lo;
  let cl_hi = vm.y | P.hdraw_hi;"""]
    prologue_lines += lane_load_stmts("boards", "P.nboards", "bbase",
                                      "cl_lo", "cl_hi", "  ")
    prologue_lines.append(
        "  // transpose lane masks -> plane words, low block then high block")
    prologue_lines += transpose_stmts("a", "  ")
    prologue_lines += transpose_stmts("b", "  ")
    prologue = "\n".join(prologue_lines) + "\n"

    if style == "holdem":
        body_lines = ["""  var x: array<word, 52>;
  var yh: array<word, 24>;
  var yv: array<word, 24>;
  // hero eval: per-lane board planes | revealed board | hero (uniform)
  let hu_lo = P.board_lo | P.hero_lo;
  let hu_hi = P.board_hi | P.hero_hi;"""]
        for p in range(32):
            body_lines.append(f"  x[{p}] = a{31 - p} | select(ZERO, ONES, ((hu_lo >> {p}u) & 1u) == 1u);")
        for q in range(20):
            body_lines.append(f"  x[{32 + q}] = b{31 - q} | select(ZERO, ONES, ((hu_hi >> {q}u) & 1u) == 1u);")
        body_lines.append("""  high_circuit(&x, &yh);
  // villain eval: same board planes, villain holes = known | drawn
  let vu_lo = P.board_lo | P.vill_lo | vm.x;
  let vu_hi = P.board_hi | P.vill_hi | vm.y;""")
        for p in range(32):
            body_lines.append(f"  x[{p}] = a{31 - p} | select(ZERO, ONES, ((vu_lo >> {p}u) & 1u) == 1u);")
        for q in range(20):
            body_lines.append(f"  x[{32 + q}] = b{31 - q} | select(ZERO, ONES, ((vu_hi >> {q}u) & 1u) == 1u);")
        body_lines.append("  high_circuit(&x, &yv);")
        body = "\n".join(body_lines) + "\n"
    else:
        decls = """  var x: array<word, 104>;
  var yh: array<word, 24>;
  var yv: array<word, 24>;
"""
        if hilo:
            decls += """  var lh: array<word, 8>;
  var lv: array<word, 8>;
"""
        body_lines = [decls +
                      "  // board planes 52..103: per-lane board | revealed board (uniform)"]
        for p in range(32):
            body_lines.append(f"  x[{52 + p}] = a{31 - p} | select(ZERO, ONES, ((P.board_lo >> {p}u) & 1u) == 1u);")
        for q in range(20):
            body_lines.append(f"  x[{84 + q}] = b{31 - q} | select(ZERO, ONES, ((P.board_hi >> {q}u) & 1u) == 1u);")
        body_lines.append(
            "  for (var p = 0u; p < 52u; p++) { x[p] = maskbit(P.hero_lo, P.hero_hi, p); }")
        body_lines.append("  high_circuit(&x, &yh);")
        if hilo:
            body_lines.append("  low_circuit(&x, &lh);")
        body_lines.append(
            "  for (var p = 0u; p < 52u; p++) { x[p] = maskbit(P.vill_lo | vm.x, P.vill_hi | vm.y, p); }")
        body_lines.append("  high_circuit(&x, &yv);")
        if hilo:
            body_lines.append("  low_circuit(&x, &lv);")
        body = "\n".join(body_lines) + "\n"

    if not hilo:
        tail = """  let ch = cmp24(&yh, &yv);
  let win = ch[0] & valid;
  let tie = ch[1] & valid;
  tadd(0u, countOneBits(win));
  tadd(1u, countOneBits(tie));
  tadd(2u, countOneBits(valid));
}
"""
    else:
        # pot shares in quarter-pot units: each half contributes 0/1/2
        # quarters; with no qualifying low the high result claims both halves
        tail = """  let ch = cmp24(&yh, &yv);
  let winH = ch[0] & valid;
  let tieH = ch[1] & valid;
  let cl = cmp8(&lv, &lh);       // villain's low larger == hero's low better
  let heroL = cl[0] & valid;
  let eqL = cl[1];
  var lhAll = lh[0];
  for (var i = 1u; i < 8u; i++) { lhAll &= lh[i]; }
  let noLow = eqL & lhAll;       // equal lows at 0xff: nobody qualifies
  let hasLow = ~noLow;
  let qH1 = winH;                // high half: 2 quarters win, 1 tie
  let qH0 = tieH;
  let qL1 = (heroL & hasLow) | (winH & noLow);
  let qL0 = ((eqL & hasLow) & valid) | (tieH & noLow);
  let s0 = qH0 ^ qL0;            // per-lane 2-bit + 2-bit add
  let c0 = qH0 & qL0;
  let s1 = qH1 ^ qL1 ^ c0;
  let s2 = (qH1 & qL1) | (c0 & (qH1 ^ qL1));
  tadd(0u, countOneBits(s0) + 2u * countOneBits(s1) + 4u * countOneBits(s2));
  tadd(1u, countOneBits(valid));
  // per-half breakdown for the UI -- every mask already exists above
  tadd(2u, countOneBits(winH));
  tadd(3u, countOneBits(tieH));
  tadd(4u, countOneBits(heroL & hasLow));
  tadd(5u, countOneBits(eqL & hasLow & valid));
  tadd(6u, countOneBits(noLow & valid));
  tadd(7u, countOneBits(winH & (noLow | (heroL & hasLow))));  // hero scoops
}
"""

    with open(out_path, "w") as f:
        f.write(header + "\n\n" + "\n\n".join(parts) + "\n\n"
                + prologue + body + tail)

    print(f"equity {style}{' hi-lo' if hilo else ''}: nin={nin} -> {out_path}")


if __name__ == "__main__":
    main()
