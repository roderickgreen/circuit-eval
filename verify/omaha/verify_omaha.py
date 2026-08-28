#!/usr/bin/env python3
"""Verify the Omaha value circuit against the validated structural prototype.

Bit-parallel simulates the BLIF over deal families chosen to hit every
category and split shape, comparing all 24 output bits per deal.

usage (from repo root): verify_omaha.py <omaha_value.blif> [ndeals_per_family] [nhole]
nhole = 4 (PLO4, default), 5 (PLO5), 6 (PLO6); the same circuit serves all.
"""
import random
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bitsim import parse_blif, simulate
from omaha_proto import structural

blif = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
NH = int(sys.argv[3]) if len(sys.argv) > 3 else 4
ND = NH + 5

random.seed(0xC0FFEE)
DECK = [(r, s) for r in range(13) for s in range(4)]


def fam_random():
    return random.sample(DECK, ND)


def fam_rankclust():
    ranks = random.sample(range(13), random.randint(2, 5))
    pool = [c for c in DECK if c[0] in ranks]
    return random.sample(pool, ND) if len(pool) >= ND else None


def fam_suitclust():
    suits = random.sample(range(4), random.randint(1, 2))
    pool = [c for c in DECK if c[1] in suits]
    return random.sample(pool, ND) if len(pool) >= ND else None


def fam_sf():
    # narrow rank span in <=2 suits: forces straight/flush/SF interplay
    lo = random.randint(0, 6)
    span = list(range(lo, min(13, lo + random.randint(5, 7))))
    if random.random() < 0.3:
        span.append(12)  # wheel ace
    suits = random.sample(range(4), random.randint(1, 2))
    pool = [c for c in DECK if c[0] in span and c[1] in suits]
    return random.sample(pool, ND) if len(pool) >= ND else None


def fam_pocket():
    # pocket-pair-heavy holes: stresses split feasibility (a=2 cases)
    r1, r2 = random.sample(range(13), 2)
    hole = [(r1, s) for s in random.sample(range(4), 2)] + \
           [(r2, s) for s in random.sample(range(4), 2)]
    rest = [c for c in DECK if c not in hole]
    return hole + random.sample(rest, ND - 4)


def fam_boardclust():
    # board with 3-4 of one rank: quads/trips-on-board shapes
    r = random.randrange(13)
    k = random.randint(3, 4)
    board = [(r, s) for s in random.sample(range(4), k)]
    rest = [c for c in DECK if c not in board]
    board += random.sample(rest, 5 - k)
    rest = [c for c in DECK if c not in board]
    return random.sample(rest, NH) + board


FAMS = [("random", fam_random), ("rankclust", fam_rankclust),
        ("suitclust", fam_suitclust), ("sf", fam_sf),
        ("pocket", fam_pocket), ("boardclust", fam_boardclust)]

deals, want = [], []
for name, gen in FAMS:
    got = 0
    while got < N:
        d = gen()
        if d is None:
            continue
        hole, board = d[:NH], d[NH:]
        deals.append((hole, board))
        want.append(structural(hole, board))
        got += 1

nvec = len(deals)
# ABC's deepsyn renames PIs/POs (pi000...); order is preserved, so map
# positionally: .inputs = h0..h51 b0..b51, .outputs = ov0..ov23
ins, outs, _ = parse_blif(blif)
assert len(ins) == 104 and len(outs) == 24, (len(ins), len(outs))
invals = {w: 0 for w in ins}
for i, (hole, board) in enumerate(deals):
    for r, s in hole:
        invals[ins[4 * r + s]] |= 1 << i
    for r, s in board:
        invals[ins[52 + 4 * r + s]] |= 1 << i

val, _ = simulate(blif, invals, nvec)
bad = 0
for i in range(nvec):
    got = sum(((val[outs[b]] >> i) & 1) << b for b in range(24))
    if got != want[i]:
        bad += 1
        if bad <= 10:
            h, b = deals[i]
            print(f"MISMATCH hole={h} board={b} want={want[i]:#08x} got={got:#08x}")
print(f"{nvec} deals ({N} x {len(FAMS)} families), {bad} mismatches")
sys.exit(1 if bad else 0)
