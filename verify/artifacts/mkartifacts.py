#!/usr/bin/env python3
"""Ground-truth artifact generator.

Writes:
  classes.csv        class_id, category, reference value (hex), description
  rank_multiset.bin  int32 count, then records of uint64 packed rank-count key
                     (3 bits per rank, rank r at bit 3r), uint16 class_id;
                     ascending key. Non-flush hands only.
  flush_mask.bin     int32 count, then records of uint16 13-bit flush-suit
                     rank mask, uint16 class_id; ascending mask.

The reference value (category<<24 | monotone tiebreak nibbles) is the repo's
encoding contract. Ordering ground truth is off-the-shelf: after generating,
run the exhaustive C(52,7) sweep against PokerHandEvaluator, which checks that
the table path is a strict order-isomorphism with PHE on every hand:

  make -C verify build/fullsweep && verify/build/fullsweep

usage (from repo root): mkartifacts.py [outdir]      default outdir: artifacts
"""
import struct
import sys
from functools import lru_cache

CATEGORIES = [
    "HighCard", "OnePair", "TwoPair", "ThreeOfAKind", "Straight",
    "Flush", "FullHouse", "FourOfAKind", "StraightFlush",
]
# Distinct best-5-of-7 strengths per category (2+2 forum / standard result).
EXPECTED_CLASS_COUNTS = [407, 1470, 763, 575, 10, 1277, 156, 156, 10]
RANK_CHARS = "23456789TJQKA"


def straight_high(rank_mask):
    """Highest straight top-rank in a 13-bit rank mask, or -1. Wheel returns 3."""
    for high in range(12, 3, -1):
        if (rank_mask >> (high - 4)) & 0x1F == 0x1F:
            return high
    if rank_mask & 0x100F == 0x100F:
        return 3
    return -1


def pack_ranks_desc(rank_mask):
    acc = 0
    for r in range(12, -1, -1):
        if (rank_mask >> r) & 1:
            acc = (acc << 4) | r
    return acc


@lru_cache(maxsize=None)
def eval5_ranks(ranks):
    """Reference value of a flushless 5-card hand given its (sorted) ranks."""
    cnt = [0] * 13
    rank_mask = 0
    for r in ranks:
        cnt[r] += 1
        rank_mask |= 1 << r
    sh = straight_high(rank_mask)

    quad = trip = pair_hi = pair_lo = -1
    for r in range(12, -1, -1):
        c = cnt[r]
        if c == 4:
            quad = r
        elif c == 3:
            trip = r
        elif c == 2:
            if pair_hi < 0:
                pair_hi = r
            elif pair_lo < 0:
                pair_lo = r

    def singles_desc(exclude, take):
        acc = taken = 0
        for r in range(12, -1, -1):
            if taken == take:
                break
            if r != exclude and cnt[r] == 1:
                acc = (acc << 4) | r
                taken += 1
        return acc

    if quad >= 0:
        return (7 << 24) | (quad << 4) | singles_desc(quad, 1)
    if trip >= 0 and pair_hi >= 0:
        return (6 << 24) | (trip << 4) | pair_hi
    if sh >= 0:
        return (4 << 24) | sh
    if trip >= 0:
        return (3 << 24) | (trip << 8) | singles_desc(trip, 2)
    if pair_lo >= 0:
        return (2 << 24) | (pair_hi << 8) | (pair_lo << 4) | singles_desc(-1, 1)
    if pair_hi >= 0:
        return (1 << 24) | (pair_hi << 12) | singles_desc(-1, 3)
    return pack_ranks_desc(rank_mask)  # high card, category 0


def eval7_counts(counts):
    """Best-5-of-7 value of a non-flush hand given its 13 rank counts.

    A 7-card hand whose ranks realize these counts can always be suited
    flush-free (per-rank suits distinct, greedy least-loaded keeps every suit
    at <= ceil(7/4) = 2 cards), so no 5-card subset triggers the flush branch
    and the value depends on ranks alone.
    """
    ranks = []
    for r in range(13):
        ranks.extend([r] * counts[r])
    best = 0
    for i in range(7):
        for j in range(i + 1, 7):
            five = tuple(sorted(ranks[m] for m in range(7) if m != i and m != j))
            v = eval5_ranks(five)
            if v > best:
                best = v
    return best


def eval_flush_mask(rank_mask):
    """Best value of a 5..7 card single-suit holding given its 13-bit rank mask."""
    sh = straight_high(rank_mask)
    if sh >= 0:
        return (8 << 24) | sh
    acc = taken = 0
    for r in range(12, -1, -1):
        if taken == 5:
            break
        if (rank_mask >> r) & 1:
            acc = (acc << 4) | r
            taken += 1
    return (5 << 24) | acc


def describe(v):
    t = v & 0xFFFFFF

    def R(shift):
        return RANK_CHARS[(t >> shift) & 0xF]

    cat = v >> 24
    if cat == 0:
        return f"High Card {R(16)} {R(12)} {R(8)} {R(4)} {R(0)}"
    if cat == 1:
        return f"One Pair {R(12)}{R(12)} + {R(8)} {R(4)} {R(0)}"
    if cat == 2:
        return f"Two Pair {R(8)}{R(8)} {R(4)}{R(4)} + {R(0)}"
    if cat == 3:
        return f"Trips {R(8)}{R(8)}{R(8)} + {R(4)} {R(0)}"
    if cat == 4:
        return f"Straight to {R(0)}"
    if cat == 5:
        return f"Flush {R(16)} {R(12)} {R(8)} {R(4)} {R(0)}"
    if cat == 6:
        return f"Full House {R(4)}{R(4)}{R(4)} over {R(0)}{R(0)}"
    if cat == 7:
        return f"Quads {R(4)}{R(4)}{R(4)}{R(4)} + {R(0)}"
    if cat == 8:
        return f"Straight Flush to {R(0)}"
    return f"?0x{v:08X}"


def main():
    import os
    import time

    out_dir = sys.argv[1] if len(sys.argv) > 1 else "artifacts"
    t0 = time.time()
    print("Building ground-truth tables...")

    # Rank multisets: 13 counts in 0..4 summing to 7, key = 3 bits per rank.
    multiset_value = {}

    def recurse(rank, remaining, key):
        if rank == 12:
            if remaining <= 4:
                counts[12] = remaining
                multiset_value[key | (remaining << 36)] = eval7_counts(counts)
                counts[12] = 0
            return
        for c in range(min(4, remaining) + 1):
            counts[rank] = c
            recurse(rank + 1, remaining - c, key | (c << (3 * rank)))
        counts[rank] = 0

    counts = [0] * 13
    recurse(0, 7, 0)

    # Flush masks: 13-bit masks with 5..7 bits set.
    flush_value = {}
    for mask in range(1 << 13):
        if 5 <= bin(mask).count("1") <= 7:
            flush_value[mask] = eval_flush_mask(mask)

    # Distinct achievable values, densely numbered 0 (worst) .. N-1 (best).
    class_values = sorted(set(multiset_value.values()) | set(flush_value.values()))
    value_to_class = {v: i for i, v in enumerate(class_values)}

    print(
        f"  {len(multiset_value):,} rank multisets, {len(flush_value):,} flush masks, "
        f"{len(class_values):,} distinct classes  ({time.time() - t0:.1f}s)"
    )

    ok = True
    if len(multiset_value) != 49_205:
        print("  MISMATCH: expected 49,205 rank multisets")
        ok = False
    if len(flush_value) != 4_719:
        print("  MISMATCH: expected 4,719 flush masks")
        ok = False

    class_counts = [0] * 9
    for v in class_values:
        class_counts[v >> 24] += 1
    print()
    print("  Distinct classes by category:")
    for c in range(9):
        flag = "ok" if class_counts[c] == EXPECTED_CLASS_COUNTS[c] else \
            f"MISMATCH (expected {EXPECTED_CLASS_COUNTS[c]})"
        print(f"    {CATEGORIES[c]:<14} {class_counts[c]:>5,}  {flag}")
        if class_counts[c] != EXPECTED_CLASS_COUNTS[c]:
            ok = False
    total_flag = "ok" if len(class_values) == 4824 else "MISMATCH (expected 4,824)"
    print(f"    {'total':<14} {len(class_values):>5,}  {total_flag}")
    if len(class_values) != 4824:
        ok = False

    if not ok:
        print()
        print("VALIDATION FAILED -- artifacts not written.")
        return 1

    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "classes.csv"), "w") as f:
        f.write("class_id,category,ref_value_hex,description\n")
        for i, v in enumerate(class_values):
            f.write(f"{i},{CATEGORIES[v >> 24]},{v:08X},{describe(v)}\n")

    with open(os.path.join(out_dir, "rank_multiset.bin"), "wb") as f:
        f.write(struct.pack("<i", len(multiset_value)))
        for key in sorted(multiset_value):
            f.write(struct.pack("<QH", key, value_to_class[multiset_value[key]]))

    with open(os.path.join(out_dir, "flush_mask.bin"), "wb") as f:
        f.write(struct.pack("<i", len(flush_value)))
        for mask in sorted(flush_value):
            f.write(struct.pack("<HH", mask, value_to_class[flush_value[mask]]))

    print()
    print(f"Artifacts written to {os.path.abspath(out_dir)}")
    print("Now validate exhaustively vs PHE: "
          "make -C verify build/fullsweep && verify/build/fullsweep")
    return 0


if __name__ == "__main__":
    sys.exit(main())
