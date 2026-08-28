#!/usr/bin/env python3
"""Verify the low (8-or-better) circuit against a from-cards brute force.

The oracle enumerates actual card picks (2 of hole x 3 of board, 5 distinct
low ranks, min mask) -- independent of the generator's presence-bit theory,
so it would catch an error in the sufficient-statistic argument itself.

Two sweeps, one bit-parallel simulation:
  1. EXHAUSTIVE over realizable rank-set pairs: every (H, B) with
     |H| <= 6, |B| <= 5 (54,093 pairs), realized as a concrete deal with
     random suits, random duplicate-rank padding (multiplicity noise) and
     random high-card padding. Covers the complete input domain of the
     16-bit core reachable by any deal in any variant.
  2. Random full deals at nhole = 4, 5, 6 (reduction-layer / plane-map
     coverage on unconstrained inputs).

usage (from repo root): verify_low.py <low.blif> [nrandom_per_variant]
"""
import itertools
import random
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec"))
from bitsim import parse_blif, simulate

blif = sys.argv[1]
NRAND = int(sys.argv[2]) if len(sys.argv) > 2 else 20000

random.seed(0x10B0)
DECK = [(r, s) for r in range(13) for s in range(4)]
LOWIDX = {12: 0, 0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 6: 7}  # deck rank -> low bit
DECK_RANK = [12, 0, 1, 2, 3, 4, 5, 6]  # low bit -> deck rank
HIGHRANKS = list(range(7, 12))  # 9,T,J,Q,K: never part of a low


def best_low_cards(hole, board):
    best = 0xFF
    for hc in itertools.combinations(hole, 2):
        for bc in itertools.combinations(board, 3):
            ranks = {c[0] for c in hc} | {c[0] for c in bc}
            if len(ranks) != 5 or not all(r in LOWIDX for r in ranks):
                continue
            mask = sum(1 << LOWIDX[r] for r in ranks)
            best = min(best, mask)
    return best


def realize(H, B):
    """Concrete deal whose low-rank presence sets are exactly (H, B)."""
    used = set()

    def take(r):
        s = random.choice([s for s in range(4) if (r, s) not in used])
        used.add((r, s))
        return (r, s)

    hole = [take(DECK_RANK[i]) for i in H]
    board = [take(DECK_RANK[i]) for i in B]
    nh = random.choice([n for n in (4, 5, 6) if n >= len(H)])
    while len(hole) < nh:
        # multiplicity noise: sometimes duplicate an existing low rank
        # (another suit -- presence unchanged), else pad with a high card
        r = (random.choice(hole)[0] if H and random.random() < 0.3
             else random.choice(HIGHRANKS))
        if r in LOWIDX and all((r, s) in used for s in range(4)):
            r = random.choice(HIGHRANKS)
        if all((r, s) in used for s in range(4)):
            continue
        hole.append(take(r))
    while len(board) < 5:
        r = (random.choice(board)[0] if B and random.random() < 0.3
             else random.choice(HIGHRANKS))
        if r in LOWIDX and all((r, s) in used for s in range(4)):
            r = random.choice(HIGHRANKS)
        if all((r, s) in used for s in range(4)):
            continue
        board.append(take(r))
    return hole, board


deals = []
lowsets = [frozenset(s) for n in range(7)
           for s in itertools.combinations(range(8), n)]
boardsets = [frozenset(s) for n in range(6)
             for s in itertools.combinations(range(8), n)]
for H in lowsets:
    for B in boardsets:
        deals.append(realize(sorted(H), sorted(B)))
nex = len(deals)
for nh in (4, 5, 6):
    for _ in range(NRAND):
        d = random.sample(DECK, nh + 5)
        deals.append((d[:nh], d[nh:]))

want = [best_low_cards(h, b) for h, b in deals]

nvec = len(deals)
ins, outs, _ = parse_blif(blif)
assert len(ins) == 104 and len(outs) == 8, (len(ins), len(outs))
invals = {w: 0 for w in ins}
for i, (hole, board) in enumerate(deals):
    for r, s in hole:
        invals[ins[4 * r + s]] |= 1 << i
    for r, s in board:
        invals[ins[52 + 4 * r + s]] |= 1 << i

val, _ = simulate(blif, invals, nvec)
bad = 0
for i in range(nvec):
    got = sum(((val[outs[b]] >> i) & 1) << b for b in range(8))
    if got != want[i]:
        bad += 1
        if bad <= 10:
            h, b = deals[i]
            print(f"MISMATCH hole={h} board={b} want={want[i]:#04x} got={got:#04x}")
print(f"{nvec} deals ({nex} exhaustive rank-set pairs + 3 x {NRAND} random), "
      f"{bad} mismatches")
sys.exit(1 if bad else 0)
