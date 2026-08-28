#!/usr/bin/env python3
"""Graft a synthesized 16-input low core back onto the 104 card planes.

The core is synthesized standalone (deepsyn explores the 16-variable space
far better than the composed netlist -- 173 vs 1,420 nodes), then this
script prepends the 16 rank-presence OR trees (hp_i = any suit of low rank
i in hole, bp_i = board; 3 OR2 each = 48 gates) and renames wires. Core
PIs/POs are mapped positionally (deepsyn renames them, order preserved):
inputs hp0..hp7 bp0..bp7, outputs lo0..lo7.

usage: compose_low.py <core.blif> <out.blif>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
from bitsim import parse_blif

DECK_RANK = [12, 0, 1, 2, 3, 4, 5, 6]  # low rank index -> deck rank

ins, outs, gates = parse_blif(sys.argv[1])
assert len(ins) == 16 and len(outs) == 8, (len(ins), len(outs))

ren = {}
for i in range(8):
    ren[ins[i]] = f"hp{i}"
    ren[ins[8 + i]] = f"bp{i}"
for j in range(8):
    ren[outs[j]] = f"lo{j}"


def w(name):
    return ren.get(name, f"c_{name}")


out = [".model low_value",
       ".inputs " + " ".join(f"h{i}" for i in range(52)) + " "
       + " ".join(f"b{i}" for i in range(52)),
       ".outputs " + " ".join(f"lo{i}" for i in range(8))]
for i in range(8):
    r = DECK_RANK[i]
    out.append(".names " + " ".join(f"h{4 * r + s}" for s in range(4))
               + f" hp{i}\n1--- 1\n-1-- 1\n--1- 1\n---1 1")
    out.append(".names " + " ".join(f"b{4 * r + s}" for s in range(4))
               + f" bp{i}\n1--- 1\n-1-- 1\n--1- 1\n---1 1")
for name, gins, cover in gates:
    out.append(".names " + " ".join(w(g) for g in gins) + " " + w(name)
               + "\n" + "\n".join(cover))
out.append(".end")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
print(f"composed: {len(gates)} core gates + 16 OR4 reduction")
