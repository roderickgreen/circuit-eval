#!/usr/bin/env python3
"""Omaha structural prototype: best hand via per-category max over
(hole_count, board_count) per rank + suit masks -- no 60-combo enumeration.
Validated against the naive max-over-60 reference.

Value encoding = holdem's 24-bit: cat<<20 | n1..n5 nibbles.
cat: HC=1 1P=2 2P=3 3K=4 ST=5 FL=6 FH=7 Q=8 SF=9.
"""
import itertools
import random
import sys

# ---------- naive reference ----------
def val5(cards):  # 5 cards (rank, suit)
    rs = sorted((r for r, _ in cards), reverse=True)
    cnt = {}
    for r in rs:
        cnt[r] = cnt.get(r, 0) + 1
    flush = len(set(s for _, s in cards)) == 1
    dist = sorted(set(rs), reverse=True)
    straight = None
    if len(dist) == 5:
        if dist[0] - dist[4] == 4:
            straight = dist[0]
        elif dist == [12, 3, 2, 1, 0]:
            straight = 3  # wheel: 5-high
    def pack(cat, nibs):
        v = cat << 20
        for i, x in enumerate(nibs):
            v |= x << (16 - 4 * i)
        return v
    by = sorted(cnt.items(), key=lambda kv: (-kv[1], -kv[0]))
    if flush and straight is not None:
        return pack(9, [straight])
    if by[0][1] == 4:
        return pack(8, [by[0][0], by[1][0]])
    if by[0][1] == 3 and by[1][1] == 2:
        return pack(7, [by[0][0], by[1][0]])
    if flush:
        return pack(6, rs)
    if straight is not None:
        return pack(5, [straight])
    if by[0][1] == 3:
        return pack(4, [by[0][0], by[1][0], by[2][0]])
    if by[0][1] == 2 and by[1][1] == 2:
        return pack(3, [by[0][0], by[1][0], by[2][0]])
    if by[0][1] == 2:
        return pack(2, [by[0][0], by[1][0], by[2][0], by[3][0]])
    return pack(1, rs)

BT = list(itertools.combinations(range(5), 3))

def naive(hole, board):
    best = 0
    for p in itertools.combinations(range(len(hole)), 2):
        for t in BT:
            v = val5([hole[p[0]], hole[p[1]]] + [board[i] for i in t])
            if v > best:
                best = v
    return best

# ---------- structural evaluator ----------
def pack(cat, nibs):
    v = cat << 20
    for i, x in enumerate(nibs):
        v |= x << (16 - 4 * i)
    return v

def topk(pool, k, excl=()):
    """top k ranks from pool (list of (rank, mult)), excluding ranks in excl;
    multiplicity capped at 1 per card use (kickers are single cards) is NOT
    applied here: pool entries repeat rank per available card."""
    out = []
    if k == 0:
        return out
    for r in sorted(pool, reverse=True):
        if r in excl:
            continue
        out.append(r)
        if len(out) == k:
            break
    return out if len(out) == k else None

def structural(hole, board):
    hc = [0] * 13
    bc = [0] * 13
    hs = [[] for _ in range(4)]   # ranks per suit
    bs = [[] for _ in range(4)]
    for r, s in hole:
        hc[r] += 1
        hs[s].append(r)
    for r, s in board:
        bc[r] += 1
        bs[s].append(r)

    def can(r, a, b):  # rank r usable with a hole + b board copies
        return a <= min(hc[r], 2) and b <= min(bc[r], 3)

    # --- straight flush / flush (at most one suit qualifies) ---
    fsuit = None
    for s in range(4):
        if len(hs[s]) >= 2 and len(bs[s]) >= 3:
            fsuit = s
    sf = None
    if fsuit is not None:
        hm = set(hs[fsuit])
        bm = set(bs[fsuit])
        for hi in range(12, 2, -1):  # window high rank: A..5 (hi=3 is wheel)
            w = [12, 0, 1, 2, 3] if hi == 3 else list(range(hi - 4, hi + 1))
            nh = sum(1 for r in w if r in hm)
            nb = sum(1 for r in w if r in bm)
            if nh + nb == 5 and nh == 2:
                sf = hi
                break
        if sf is not None:
            return pack(9, [sf])

    # --- quads ---
    for r in range(12, -1, -1):
        ks = []
        if can(r, 2, 2):
            k = topk([x for x in range(13) if bc[x] and x != r], 1)
            if k: ks.append(k[0])
        if can(r, 1, 3):
            k = topk([x for x in range(13) if hc[x] and x != r], 1)
            if k: ks.append(k[0])
        if ks:
            return pack(8, [r, max(ks)])

    # --- full house: max t, then max p ---
    def fh_feasible(t, p):
        for a in range(3):  # hole cards used by trips
            c = 2 - a
            if can(t, a, 3 - a) and can(p, c, 2 - c):
                return True
        return False
    for t in range(12, -1, -1):
        if min(hc[t], 2) + min(bc[t], 3) < 3:
            continue
        for p in range(12, -1, -1):
            if p == t:
                continue
            if fh_feasible(t, p):
                return pack(7, [t, p])

    # --- flush ---
    if fsuit is not None:
        h2 = sorted(hs[fsuit], reverse=True)[:2]
        b3 = sorted(bs[fsuit], reverse=True)[:3]
        return pack(6, sorted(h2 + b3, reverse=True))

    # --- straight ---
    for hi in range(12, 2, -1):
        w = [12, 0, 1, 2, 3] if hi == 3 else list(range(hi - 4, hi + 1))
        A = sum(1 for r in w if hc[r] >= 1 and bc[r] == 0)
        B = sum(1 for r in w if bc[r] >= 1 and hc[r] == 0)
        D = sum(1 for r in w if hc[r] == 0 and bc[r] == 0)
        if D == 0 and A <= 2 and B <= 3:
            return pack(5, [hi])

    # --- trips: max t, then best kickers over splits ---
    for t in range(12, -1, -1):
        best = None
        for a in range(3):  # hole cards in the trips
            if not can(t, a, 3 - a):
                continue
            hk = [x for x in range(13) for _ in range(min(hc[x], 2)) if x != t]
            bk = [x for x in range(13) for _ in range(min(bc[x], 3)) if x != t]
            need_h, need_b = 2 - a, a
            th = topk(hk, need_h)
            tb = topk(bk, need_b)
            if th is None or tb is None:
                continue
            ks = sorted(th + tb, reverse=True)
            if best is None or ks > best:
                best = ks
        if best:
            return pack(4, [t] + best)

    # --- two pair: max hi, then lo, then kicker ---
    def pair_splits(r):
        return [(c, 2 - c) for c in range(3) if can(r, c, 2 - c)]
    for hi in range(12, -1, -1):
        if not pair_splits(hi):
            continue
        for lo in range(hi - 1, -1, -1):
            best_k = None
            for c1, d1 in pair_splits(hi):
                for c2, d2 in pair_splits(lo):
                    eh, eb = 2 - c1 - c2, 3 - d1 - d2
                    if eh < 0 or eb < 0 or eh + eb != 1:
                        continue
                    if eh:
                        pool = [x for x in range(13) if hc[x] and x not in (hi, lo)]
                    else:
                        pool = [x for x in range(13) if bc[x] and x not in (hi, lo)]
                    k = topk(pool, 1)
                    if k and (best_k is None or k[0] > best_k):
                        best_k = k[0]
            if best_k is not None:
                return pack(3, [hi, lo, best_k])

    # --- one pair: max r, then kickers over splits ---
    for r in range(12, -1, -1):
        best = None
        for c, d in pair_splits(r):
            hk = [x for x in range(13) for _ in range(min(hc[x], 2)) if x != r]
            bk = [x for x in range(13) for _ in range(min(bc[x], 3)) if x != r]
            th = topk(hk, 2 - c)
            tb = topk(bk, 3 - d)
            if th is None or tb is None:
                continue
            ks = sorted(th + tb, reverse=True)
            if best is None or ks > best:
                best = ks
        if best:
            return pack(2, [r] + best)

    # --- high card: top2 hole + top3 board ---
    h2 = sorted((r for r, _ in hole), reverse=True)[:2]
    b3 = sorted((r for r, _ in board), reverse=True)[:3]
    return pack(1, sorted(h2 + b3, reverse=True))

# ---------- validation ----------
if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    NH = int(sys.argv[2]) if len(sys.argv) > 2 else 4   # hole cards: 4/5/6
    random.seed(0x9e3779b9)
    bad = 0
    deck = [(r, s) for r in range(13) for s in range(4)]
    for i in range(n):
        deal = random.sample(deck, NH + 5)
        hole, board = deal[:NH], deal[NH:]
        a, b = naive(hole, board), structural(hole, board)
        if a != b:
            bad += 1
            if bad <= 8:
                print(f"MISMATCH hole={hole} board={board} naive={a:#x} struct={b:#x}")
    print(f"{n} random deals, {bad} mismatches")

    # adversarial: cluster ranks to force pairs/trips/quads/straights
    bad2 = 0
    for i in range(n // 2):
        ranks = random.sample(range(13), random.randint(2, 5))
        pool = [c for c in deck if c[0] in ranks]
        if len(pool) < NH + 5:
            continue
        deal = random.sample(pool, NH + 5)
        hole, board = deal[:NH], deal[NH:]
        a, b = naive(hole, board), structural(hole, board)
        if a != b:
            bad2 += 1
            if bad2 <= 8:
                print(f"ADV MISMATCH hole={hole} board={board} naive={a:#x} struct={b:#x}")
    # suit-clustered: force flushes/straight-flushes
    bad3 = 0
    for i in range(n // 2):
        suits = random.sample(range(4), random.randint(1, 2))
        pool = [c for c in deck if c[1] in suits]
        if len(pool) < NH + 5:
            continue
        deal = random.sample(pool, NH + 5)
        hole, board = deal[:NH], deal[NH:]
        a, b = naive(hole, board), structural(hole, board)
        if a != b:
            bad3 += 1
            if bad3 <= 8:
                print(f"SUIT MISMATCH hole={hole} board={board} naive={a:#x} struct={b:#x}")
    print(f"adversarial rank-clustered: {bad2} mismatches; "
          f"suit-clustered: {bad3} mismatches")
