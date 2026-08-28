#!/usr/bin/env python3
"""Rewrite a BLIF's .inputs list into natural-sorted order (a2 before a10).

Gates bind by name, so the rewrite changes nothing for name-based tools
(bitsim, cec); what it pins down is the order positional consumers see:
the bitslice/WGSL codegens index input planes by .inputs position, and
yosys's AIG input numbering (which ABC's write_blif preserves) follows
internal node creation order, not port declaration order.  Run this on a
normalized BLIF before it becomes a synthesis start or codegen source, so
every downstream positional view agrees.  .outputs is left alone (already
in declared order).

usage: orderio.py <in.blif> <out.blif>
"""
import re
import sys


def natkey(name):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", name)]


inp, outp = sys.argv[1], sys.argv[2]
raw = open(inp).read()

# join continuation lines, find the .inputs statement
lines = raw.replace("\\\n", " ").splitlines()
out = []
for ln in lines:
    tok = ln.split()
    if tok and tok[0] == ".inputs":
        names = sorted(tok[1:], key=natkey)
        stmt = ".inputs"
        cur = stmt
        parts = []
        for n in names:
            if len(cur) + 1 + len(n) > 72:
                parts.append(cur)
                cur = " " + n
            else:
                cur += " " + n
        parts.append(cur)
        out.append(" \\\n".join(parts))
    else:
        out.append(ln)
open(outp, "w").write("\n".join(out) + "\n")
print(f"orderio: {outp} inputs natural-sorted")
