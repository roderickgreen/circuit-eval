#!/usr/bin/env python3
"""valmap: dense class id -> 24-bit order-isomorphic value (cat<<20|nibbles).

Asserts the value is strictly increasing in class id (i.e. exactly
order-isomorphic to the dense encoding). Writes u32[4824] to the output.
Run from repo root: python3 verify/spec/mkvalmap.py artifacts/factored/valmap.bin
"""
import csv
import os
import struct
import sys

def nib(v, i):
    return (v >> (4 * i)) & 15

CAT = {"HighCard": 1, "OnePair": 2, "TwoPair": 3, "ThreeOfAKind": 4,
       "Straight": 5, "Flush": 6, "FullHouse": 7, "FourOfAKind": 8,
       "StraightFlush": 9}

def tuple_of(cat, v):
    if cat in ("HighCard", "Flush"):
        return [nib(v, i) for i in range(4, -1, -1)]
    if cat == "OnePair":
        return [nib(v, 3), nib(v, 2), nib(v, 1), nib(v, 0), 0]
    if cat in ("TwoPair", "ThreeOfAKind"):
        return [nib(v, 2), nib(v, 1), nib(v, 0), 0, 0]
    if cat in ("Straight", "StraightFlush"):
        return [nib(v, 0), 0, 0, 0, 0]
    return [nib(v, 1), nib(v, 0), 0, 0, 0]   # FullHouse, FourOfAKind

vals = []
with open("artifacts/classes.csv") as f:
    for r in csv.DictReader(f):
        v = int(r["ref_value_hex"], 16)
        t = tuple_of(r["category"], v)
        val = CAT[r["category"]] << 20
        for i, x in enumerate(t):
            val |= x << (16 - 4 * i)
        vals.append(val)

assert all(vals[i] < vals[i + 1] for i in range(len(vals) - 1)), \
    "valmap not strictly increasing"
os.makedirs(os.path.dirname(sys.argv[1]) or ".", exist_ok=True)
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack(f"<{len(vals)}I", *vals))
print(f"valmap: {len(vals)} classes, strictly increasing, "
      f"range [{vals[0]:#x},{vals[-1]:#x}]")
