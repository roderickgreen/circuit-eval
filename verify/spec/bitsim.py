#!/usr/bin/env python3
"""Bit-parallel BLIF simulator: one Python big-int per wire, bit i = test vector i.

Handles the BLIF subset our generators emit: .names with 0/1/- covers
(single-output-value covers, on-set or off-set), constants, \\-continuations.
"""


def parse_blif(path):
    raw = open(path).read().replace("\\\n", " ")
    inputs, outputs, gates = [], [], []
    cur = None
    for ln in raw.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        if ln.startswith("."):
            cur = None
            tok = ln.split()
            if tok[0] == ".inputs":
                inputs += tok[1:]
            elif tok[0] == ".outputs":
                outputs += tok[1:]
            elif tok[0] == ".names":
                cur = (tok[-1], tok[1:-1], [])
                gates.append(cur)
        elif cur is not None:
            cur[2].append(ln)
    return inputs, outputs, gates


def simulate(path, invals, nvec):
    """invals: {input name: big int}. Returns {wire: big int} for all wires."""
    inputs, outputs, gates = parse_blif(path)
    mask = (1 << nvec) - 1
    val = dict(invals)
    gate_of = {g[0]: g for g in gates}
    import sys
    sys.setrecursionlimit(1_000_000)

    def ev(n):
        if n in val:
            return val[n]
        oname, ins, cover = gate_of[n]
        if not ins:  # constant
            v = mask if (cover and cover[0].strip() == "1") else 0
            val[n] = v
            return v
        iv = [ev(d) for d in ins]
        res, onval = 0, 1
        for row in cover:
            pat, o = row.split()
            onval = int(o)
            term = mask
            for ch, v in zip(pat, iv):
                if ch == "1":
                    term &= v
                elif ch == "0":
                    term &= ~v & mask
            res |= term
        v = res if onval == 1 else ~res & mask
        val[n] = v
        return v

    for o in outputs:
        ev(o)
    return val, outputs
