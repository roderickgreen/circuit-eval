#!/usr/bin/env python3
"""Verify a comparator BLIF (cmp8_io / cmp24_io) against integer compare.

Bit-parallel simulates the BLIF over directed and random (a, b) pairs and
checks the gt and eq outputs against Python's (a > b) and (a == b):
  - all 65536 pairs over the low 8 bits (exhaustive carry/borrow structure)
  - equal pairs (eq path)
  - single-bit differences at every bit position, both directions
  - off-by-one neighbors (a, a+1) and (a, a-1) (longest borrow chains)
  - boundary cross pairs over {0, 1, mid, max-1, max}
  - uniform random pairs

usage (from repo root): verify_cmp.py <cmpN_io.blif> <width> [nrandom]
"""
import random
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
from bitsim import simulate

blif = sys.argv[1]
W = int(sys.argv[2])
NRAND = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
TOP = (1 << W) - 1

random.seed(0xC0FFEE)

pairs = []
pairs += [(a, b) for a in range(256) for b in range(256)]
pairs += [(a, a) for a in (random.getrandbits(W) for _ in range(2000))]
for i in range(W):
    for _ in range(64):
        a = random.getrandbits(W)
        pairs.append((a, a ^ (1 << i)))
        pairs.append((a ^ (1 << i), a))
for _ in range(2000):
    a = random.randrange(1, TOP)
    pairs.append((a, a + 1))
    pairs.append((a, a - 1))
edges = (0, 1, TOP // 2, TOP - 1, TOP)
pairs += [(a, b) for a in edges for b in edges]
pairs += [(random.getrandbits(W), random.getrandbits(W)) for _ in range(NRAND)]

CHUNK = 1 << 16
bad = 0
for lo in range(0, len(pairs), CHUNK):
    chunk = pairs[lo:lo + CHUNK]
    nvec = len(chunk)
    invals = {}
    for i in range(W):
        invals[f"a{i}"] = sum(((a >> i) & 1) << v for v, (a, b) in enumerate(chunk))
        invals[f"b{i}"] = sum(((b >> i) & 1) << v for v, (a, b) in enumerate(chunk))
    wires, _ = simulate(blif, invals, nvec)
    want_gt = sum((a > b) << v for v, (a, b) in enumerate(chunk))
    want_eq = sum((a == b) << v for v, (a, b) in enumerate(chunk))
    for name, want in (("gt", want_gt), ("eq", want_eq)):
        diff = wires[name] ^ want
        if diff:
            bad += bin(diff).count("1")
            v = diff.bit_length() - 1
            a, b = chunk[v]
            print(f"MISMATCH {name}: a={a:#x} b={b:#x} "
                  f"got {(wires[name] >> v) & 1} want {(want >> v) & 1}")

if bad:
    print(f"verify_cmp: {blif} w={W}: {bad} mismatches over {len(pairs)} pairs")
    sys.exit(1)
print(f"verify_cmp: {blif} w={W}: OK, {len(pairs)} pairs (gt and eq exact)")
