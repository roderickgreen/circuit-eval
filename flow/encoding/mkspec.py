#!/usr/bin/env python3
"""The value-encoding table generator, independent of the circuits and of
any third-party evaluator.  The encoding itself is defined in ENCODING.md.

Enumerates all 7462 five-card hand classes from first principles and emits
"<value-hex> <dense-rank>" lines, rank ascending (dense rank = the classic
Cactus-Kev/PHE scale, 1..7462, 1 = best; asserted here to be the strictly
decreasing bijection).  The output must be byte-identical to what
verify/build/dump_valmap derives from the live circuit + PHE; that diff
is the encoding-contract gate (verify/holdem/verify_holdem.sh).

usage: mkspec.py            (table to stdout; diff against dump_valmap)
"""
from itertools import combinations
from math import comb

STRAIGHT_HIGHS = list(range(3, 13))          # wheel (five-high) .. ace-high
TOP = {1: 7462, 2: 6185, 3: 3325, 4: 2467, 5: 1609,
       6: 1599, 7: 322, 8: 166, 9: 10}


def pack(cat, nibs):
    v = cat << 20
    for i, x in enumerate(nibs):
        v |= x << (16 - 4 * i)
    return v


def classes():
    for a, b, c, d, e in combinations(range(12, -1, -1), 5):
        if a - e == 4 or (a, b, c, d, e) == (12, 3, 2, 1, 0):
            continue                          # straights live in cat 5/9
        off = comb(a, 5) + comb(b, 4) + comb(c, 3) + comb(d, 2) + e \
            - min(max(a - 4, 0), 9) - (1 if a == 12 and b > 3 else 0)
        yield pack(1, [a, b, c, d, e]), TOP[1] - off      # high card
        yield pack(6, [a, b, c, d, e]), TOP[6] - off      # flush
    for p in range(13):
        for k1, k2, k3 in combinations([r for r in range(12, -1, -1) if r != p], 3):
            off = 220 * p + comb(k1 - (k1 > p), 3) \
                + comb(k2 - (k2 > p), 2) + k3 - (k3 > p)
            yield pack(2, [p, k1, k2, k3, 0]), TOP[2] - off
    for hi, lo in combinations(range(12, -1, -1), 2):
        for k in range(13):
            if k in (hi, lo):
                continue
            off = 11 * (comb(hi, 2) + lo) + k - (k > lo) - (k > hi)
            yield pack(3, [hi, lo, k, 0, 0]), TOP[3] - off
    for t in range(13):
        for k1, k2 in combinations([r for r in range(12, -1, -1) if r != t], 2):
            off = 66 * t + comb(k1 - (k1 > t), 2) + k2 - (k2 > t)
            yield pack(4, [t, k1, k2, 0, 0]), TOP[4] - off
    for cat in (5, 9):
        for h in STRAIGHT_HIGHS:
            yield pack(cat, [h, 0, 0, 0, 0]), TOP[cat] - (h - 3)
    for cat in (7, 8):                        # full house / quads: a over b
        for a in range(13):
            for b in range(13):
                if b == a:
                    continue
                off = 12 * a + b - (b > a)
                yield pack(cat, [a, b, 0, 0, 0]), TOP[cat] - off


rows = sorted(classes(), key=lambda t: t[1])
assert [r for _, r in rows] == list(range(1, 7463)), "rank set not 1..7462"
assert all(rows[i][0] > rows[i + 1][0] for i in range(len(rows) - 1)), \
    "values not strictly decreasing in rank"
for v, r in rows:
    print(f"{v:06x} {r}")
