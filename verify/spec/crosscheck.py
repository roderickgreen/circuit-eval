#!/usr/bin/env python3
"""Crosscheck a composed 52->24 evaluator BLIF against the committed
circuit on random 7-card deals (both must use x{r}_{s} inputs / e{b}
outputs).

usage (from repo root): crosscheck.py <candidate.blif> [ndeals]
"""
import random
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
from bitsim import parse_blif, simulate

cand = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 100000
REF = "flow/netlists/holdem_value_1192.blif"

random.seed(0x5EC)
deals = [random.sample(range(52), 7) for _ in range(N)]

results = {}
for path in (cand, REF):
    ins, outs, _ = parse_blif(path)
    assert len(ins) == 52 and len(outs) == 24, (path, len(ins), len(outs))
    invals = {w: 0 for w in ins}
    byname = {w: w for w in ins}
    for i, deal in enumerate(deals):
        for card in deal:
            r, s = card // 4, card % 4
            invals[byname[f"x{r}_{s}"]] |= 1 << i
    val, _ = simulate(path, invals, N)
    results[path] = [
        sum(((val[f"e{b}"] >> i) & 1) << b for b in range(24)) for i in range(N)
    ]

cats = Counter(v >> 20 for v in results[REF])
bad = sum(1 for a, b in zip(results[cand], results[REF]) if a != b)
for i, (a, b) in enumerate(zip(results[cand], results[REF])):
    if a != b:
        print(f"MISMATCH deal={sorted(deals[i])} cand={a:#08x} ref={b:#08x}")
        if bad > 10:
            break
print(f"{N} random deals, {bad} mismatches; "
      "category coverage: " + ", ".join(f"{c}:{n}" for c, n in sorted(cats.items())))
sys.exit(1 if bad else 0)
