#!/usr/bin/env python3
"""Verify the order-isomorphic modules against valmap over their exact domains.

usage (from repo root):
  verify_value.py rank  <rank_value.blif>   -- all 49,205 rank multisets
  verify_value.py flush <flush_value.blif>  -- all 4,719 flush masks
"""
import struct
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
from bitsim import simulate

mode, blif = sys.argv[1], sys.argv[2]
valmap = list(struct.unpack("<4824I", open("artifacts/factored/valmap.bin", "rb").read()))

if mode == "rank":
    d = open("artifacts/rank_multiset.bin", "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    keys, clss = [], []
    off = 4
    for _ in range(n):
        k, c = struct.unpack_from("<QH", d, off)
        off += 10
        keys.append(k)
        clss.append(c)
    invals = {}
    for r in range(13):
        for b in range(3):
            v = 0
            for i, k in enumerate(keys):
                if (k >> (3 * r + b)) & 1:
                    v |= 1 << i
            invals[f"c{r}_{b}"] = v
    pref = "rk"
else:
    d = open("artifacts/flush_mask.bin", "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    keys, clss = [], []
    off = 4
    for _ in range(n):
        m, c = struct.unpack_from("<HH", d, off)
        off += 4
        keys.append(m)
        clss.append(c)
    invals = {}
    for r in range(13):
        v = 0
        for i, m in enumerate(keys):
            if (m >> r) & 1:
                v |= 1 << i
        invals[f"m{r}"] = v
    pref = "fl"

val, _ = simulate(blif, invals, n)
bad = 0
for i in range(n):
    got = sum(((val[f"{pref}{b}"] >> i) & 1) << b for b in range(24))
    if got != valmap[clss[i]]:
        bad += 1
        if bad <= 10:
            print(f"MISMATCH key={keys[i]:x} cls={clss[i]} "
                  f"want={valmap[clss[i]]:#x} got={got:#x}")
print(f"{n} {mode} inputs, {bad} mismatches")
sys.exit(1 if bad else 0)
