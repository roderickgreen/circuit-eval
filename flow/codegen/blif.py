"""Shared BLIF front end for the compiler backends (cpu/ and webgpu/).

Reads the logic-network BLIF dialect ABC's write_blif produces: .model /
.inputs / .outputs headers (with '\' line continuations) and .names gates
whose covers are SOP lines "<pattern> <value>" -- one product term per
line, '1'/'0'/'-' meaning input asserted / negated / don't-care, all lines
sharing one output value (1 = the OR of the terms drives the output, 0 =
its complement does).

Every backend consumes the same three steps -- parse, topological order,
cover-to-expression -- and differs only in what it wraps around the
resulting statement list, so those steps live here:

  parse(path)         -> (model, inputs, outputs, gates)
                         gates: out wire -> (in wires, cover lines)
  topo(...)           -> gate output wires in dependency order
  sched(...)          -> the same gates, ordered for register pressure
                         (see its docstring)
  gate_expr(...)      -> one gate's cover as a C/WGSL bitwise expression
  gate_truth(...)     -> one gate's cover as a truth table
  lut3_cover(...)     -> the same network re-covered in 3-input gates
                         (bitslice.py only; see its docstring)
  ternlog_imm(...)    -> the immediate for a 3-input gate as a ternary-
  needs_ternlog(...)     logic instruction, and whether one is needed
                         (bitslice.py only)
  card_planes(inputs) -> input position of card 4*rank+suit, or None
"""
import itertools
import sys


def parse(path):
    model = "circuit"
    inputs, outputs = [], []
    gates = {}  # out wire -> (in wires, cover lines)
    cur = None
    for ln in open(path).read().replace("\\\n", " ").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if ln.startswith("."):
            cur = None
            tok = ln.split()
            if tok[0] == ".model":
                if len(tok) > 1:
                    model = tok[1]
            elif tok[0] == ".inputs":
                inputs += tok[1:]
            elif tok[0] == ".outputs":
                outputs += tok[1:]
            elif tok[0] == ".names":
                cur = (tok[1:-1], [])
                gates[tok[-1]] = cur
            elif tok[0] != ".end":
                sys.exit(f"unsupported construct: {tok[0]}")
        elif cur is not None:
            cur[1].append(ln)
    return model, inputs, outputs, gates


def topo(inputs, outputs, gates):
    """Gate output wires in dependency order (iterative DFS from the
    outputs; a wire in progress seen again means a combinational cycle)."""
    in_set = set(inputs)
    order, state = [], {}  # wire -> 0 in progress, 1 emitted
    for root in outputs:
        stack = [(root, False)]
        while stack:
            n, ready = stack.pop()
            if ready:
                state[n] = 1
                order.append(n)
            elif n not in in_set and state.get(n) != 1:
                if state.get(n) == 0:
                    sys.exit(f"cycle at {n}")
                state[n] = 0
                stack.append((n, True))
                stack += [(d, False) for d in gates[n][0]]
    return order


def _tables(inputs, outputs, gates):
    """Dependency tables shared by the scheduler passes.

    Everything is built off topo()'s list rather than off a set, so codegen
    is byte-identical run to run -- iterating a set of wire names would make
    the emitted circuit depend on Python's string hash seed.
    """
    live = topo(inputs, outputs, gates)     # same gates, and prunes dead logic
    in_set, keep = set(inputs), set(live)

    preds, users = {}, {g: [] for g in live}
    for g in live:                          # stable dedup, source order
        seen, ps = set(), []
        for d in gates[g][0]:
            if d not in seen and (d in keep or d in in_set):
                seen.add(d)
                ps.append(d)
        preds[g] = ps
    for g in live:
        for d in preds[g]:
            if d not in in_set:
                users[d].append(g)

    # every wire gets a distinct rank, inputs included, so any tie broken on
    # it is broken the same way on every run
    rank = {w: -(k + 1) for k, w in enumerate(inputs)}
    rank.update({g: i for i, g in enumerate(live)})

    return live, in_set, set(outputs), preds, users, rank


def _list_schedule(tables):
    """Emit ready gates, preferring the one that ends the most live ranges
    (operands no later gate reads), then the one latest in topological
    order.  Only internal wires count: the scalar backend reads an input
    from the input array at each use (BS_KEEP in bitslice.py), so an
    input held live occupies no register the allocator has to spill.
    Nothing else enters the key -- not the operands a gate leaves live,
    not its arity, not its depth."""
    live, in_set, _, preds, users, rank = tables
    remaining = {g: sum(1 for d in preds[g] if d not in in_set) for g in live}
    pending = {g: len(users[g]) for g in live}
    ready = [g for g in live if remaining[g] == 0]

    def key(g):
        dying = sum(1 for d in preds[g] if d not in in_set and pending[d] == 1)
        return (-dying, -rank[g])

    order = []
    while ready:
        best = min(ready, key=key)
        ready.remove(best)
        order.append(best)
        for d in preds[best]:
            if d not in in_set:
                pending[d] -= 1
        for u in users[best]:
            remaining[u] -= 1
            if remaining[u] == 0:
                ready.append(u)
    assert len(order) == len(live), "scheduler dropped gates"
    return order


def sched(inputs, outputs, gates):
    """Same gates as topo(), ordered to hold down register pressure.

    The scalar backend gives every wire its own local, so this order is
    what a register allocator starts from.  A plain topological walk
    leaves values live far from their consumers: the live set peaks at
    hundreds of wires against the 16 or 32 vector registers a target has,
    and the allocator spills heavily.  Pressure still exceeds the register
    file after scheduling, so this reduces spilling, it does not remove
    it.
    """
    return _list_schedule(_tables(inputs, outputs, gates))


def gate_expr(ins, cover, ref, ones="ONES", zero="ZERO"):
    """Render one gate's cover as an expression: OR of AND terms over
    ref(wire), with ~ for negated literals. Multi-literal AND terms inside
    an OR are parenthesized (C precedence is right anyway; WGSL forbids
    mixing & and | without parentheses)."""
    if not ins:  # constant gate
        return ones if any(l.strip() == "1" for l in cover) else zero
    terms, val = [], "1"
    for l in cover:
        pat, val = (l.split() + ["1"])[:2]
        lits = [("~" if c == "0" else "") + ref(w)
                for c, w in zip(pat, ins) if c != "-"]
        terms.append(" & ".join(lits) if lits else ones)
    e = " | ".join(f"({t})" if len(terms) > 1 and " & " in t else t
                   for t in terms)
    return f"~({e})" if val == "0" else e


def gate_truth(ins, cover):
    """A gate's cover as a truth table: {input bits tuple: output bool}."""
    if not ins:                          # constant gate
        return {(): any(l.strip() == "1" for l in cover)}
    pats, val = [], "1"
    for l in cover:
        pat, val = (l.split() + ["1"])[:2]
        pats.append(pat)
    tt = {}
    for bits in itertools.product((0, 1), repeat=len(ins)):
        on = any(all(c == "-" or int(c) == b for c, b in zip(pat, bits))
                 for pat in pats)
        tt[bits] = on if val == "1" else not on
    return tt


def _truth_cover(ins, tt):
    """Inverse of gate_truth: SOP lines for sched()/topo(), one minterm per
    line over whichever output value has fewer of them."""
    ones = [b for b, v in tt.items() if v]
    zeros = [b for b, v in tt.items() if not v]
    if not ones:
        return [" 0"]
    if not zeros or len(ones) <= len(zeros):
        return ["".join(map(str, b)) + " 1" for b in ones]
    return ["".join(map(str, b)) + " 0" for b in zeros]


def lut3_cover(inputs, outputs, gates):
    """Re-cover the network in gates of at most three inputs.

    A gate whose output feeds exactly one other gate, and is not a circuit
    output, is absorbed into that consumer when the union of their inputs
    is at most three wires; the consumer becomes one 3-input function and
    the absorbed gate is no longer emitted.  Gates with several consumers
    are left alone -- absorbing one would recompute it per consumer and
    keep its inputs live across all of them.  The absorbed gate's inputs
    stay live until the consumer instead of until the gate, which is the
    same point when the gate is scheduled next to its consumer.

    Returns (gates3, truth, absorbed):
      gates3   out wire -> (in wires, cover lines), the covered network in
               parse() format, for sched()/topo()/gate_expr()
      truth    out wire -> truth table over gates3[out][0] (gate_truth form)
      absorbed the wires no longer emitted (each is a key of `gates`)
    Only gates topo() reaches from the outputs are covered; dead logic is
    dropped here as it is there."""
    live = topo(inputs, outputs, gates)
    in_set, out_set = set(inputs), set(outputs)
    G = {g: (list(gates[g][0]), gate_truth(*gates[g])) for g in live}
    users = {g: 0 for g in live}
    for g in live:
        for d in G[g][0]:
            if d not in in_set:
                users[d] += 1

    def absorb(g, a):
        gi, gtt = G[g]
        ai, att = G[a]
        ins = list(dict.fromkeys([w for w in gi if w != a] + ai))
        tt = {}
        for bits in itertools.product((0, 1), repeat=len(ins)):
            env = dict(zip(ins, bits))
            env[a] = int(att[tuple(env[w] for w in ai)])
            tt[bits] = gtt[tuple(env[w] for w in gi)]
        return ins, tt

    absorbed = set()
    for g in live:                        # topological: preds are final
        changed = True
        while changed:
            changed = False
            for a in G[g][0]:
                if a in in_set or a in out_set or a in absorbed:
                    continue
                if users[a] != 1:
                    continue
                ins, tt = absorb(g, a)
                if len(ins) <= 3:
                    G[g] = (ins, tt)
                    absorbed.add(a)
                    changed = True
                    break
    kept = [g for g in live if g not in absorbed]
    gates3 = {g: (G[g][0], _truth_cover(*G[g])) for g in kept}
    truth = {g: G[g][1] for g in kept}
    return gates3, truth, absorbed


def ternlog_imm(ops, ins, tt):
    """The 8-bit immediate that makes a ternary-logic instruction over
    operands ops = (A, B, C) compute tt (a table over ins, a subset of ops):
    bit 4a+2b+c of the immediate is the function value at A=a, B=b, C=c."""
    imm = 0
    for a, b, c in itertools.product((0, 1), repeat=3):
        env = {ops[0]: a, ops[1]: b, ops[2]: c}
        if tt[tuple(env[w] for w in ins)]:
            imm |= 1 << (4 * a + 2 * b + c)
    return imm


def needs_ternlog(ins, tt):
    """True when the function is not a single AND / ANDN / OR / XOR of two
    wires -- the two-input forms a vector ISA provides as one non-destructive
    instruction -- so emitting it as anything else costs two instructions."""
    if len(ins) == 3:
        return True
    if len(ins) < 2:
        return False
    ones = sum(tt.values())
    if ones == 1:                # AND / ANDN; NOR (~a & ~b) is not one op
        return tt[(0, 0)]
    if ones == 3:                # OR; NAND / ORN invert an input or the output
        return tt[(0, 0)]
    if ones == 2:                # XOR; XNOR needs the complement
        return tt[(0, 0)]
    return False


def card_planes(inputs):
    """card id (4*rank+suit) -> input position, from x{rank}_{suit} input
    names. None when the circuit uses another input convention (e.g. the
    omaha positional planes) -- callers fall back to positional order."""
    planes = [None] * 52
    try:
        for pos, name in enumerate(inputs):
            r, s = name[1:].split("_")
            planes[4 * int(r) + int(s)] = pos
    except ValueError:
        return None
    return planes
