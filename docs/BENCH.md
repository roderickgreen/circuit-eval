# Benchmark results

Six runs of `make -C bench report`, on three machines.

- EC2 c8a.2xlarge -- 8 cores (no SMT) of an AMD EPYC 9R45 (Zen 5),
  AVX-512 with the full 512-bit datapath. 48 KiB L1d + 1 MiB L2 per
  core, 32 MiB shared L3. Perf counters exposed.
- EC2 c6a.4xlarge -- 8 cores (no SMT) of an AMD EPYC 7R13 (Zen 3), AVX2.
  32 KiB L1d + 512 KiB L2 per core, 32 MiB shared L3. No perf counters.
- MacBook Air (Apple M5) -- 10 cores, arm64/NEON. 64 KiB L1d, 6 MiB L2
  per cluster. Run under `taskpolicy -c utility` on an otherwise quiet
  machine.

Nothing below compares the three machines with each other.

The six builds, and the column names the tables use:

| Column     | Machine     | Compiler       | -march           | BS_BATCH | Conversion layer   | Run |
|------------|-------------|----------------|------------------|---------:|--------------------|------------|
| Zen 5 512  | c8a.2xlarge | gcc 15.2       | native (AVX-512) |      512 | AVX-512 intrinsics | 2026-08-23 |
| Zen 5 256  | c8a.2xlarge | gcc 15.2       | x86-64-v3        |      256 | AVX2 intrinsics    | 2026-08-23 |
| Zen 3 256  | c6a.4xlarge | gcc 15.2       | native (AVX2)    |      256 | AVX2 intrinsics    | 2026-08-23 |
| Zen 3 port | c6a.4xlarge | gcc 15.2       | native (AVX2)    |      256 | portable           | 2026-08-23 |
| M5 NEON    | M5 Air      | apple clang 21 | native (arm64)   |      128 | NEON intrinsics    | 2026-08-23 |
| M5 port    | M5 Air      | apple clang 21 | native (arm64)   |      128 | portable           | 2026-08-23 |

`-march` applies to the whole build, the other evaluators included.
"portable" is the same build with `PACK=portable MASKS=portable`: mask
building and the transposes compiled from generic vector C instead of
intrinsics.

Harness: Google Benchmark, 25 repetitions, random interleaving, one
thread, pinned to one core with `taskset` on the EC2 boxes. Numbers are
per-repetition medians of ns/hand. The other evaluators build at -O3,
our library at -O2 (the flags `lib/Makefile` uses), from the same
-march. A pool of 2^20 hands (or deals) is generated off the clock; one
iteration streams the whole pool through an evaluator, folding every
value into an accumulator.

The evaluators:

- CircuitEval -- this repo, through the library API (mask building,
  transpose, circuit, value unpack)
- TwoPlusTwo -- 7 chained lookups into a 124 MB table (holdem only)
- OMPEval -- incremental Hand objects + perfect-hash lookup
- PHE (PokerHandEvaluator) -- perfect-hash evaluators; the only other
  library with dedicated Omaha (plo4/5/6) evaluators

## Main battery, holdem (bench/holdem.cc)

Every evaluator starts from the same input, a list of seven card ids per
hand, and builds whatever it needs on the clock with no state across
hands. Random is uniformly sampled hands (fixed-seed LCG), sequential is
colex enumeration order. ns/hand:

| Evaluator   | Order  | Zen 5 512 | Zen 5 256 | Zen 3 256 | Zen 3 port | M5 NEON | M5 port |
|-------------|--------|----------:|----------:|----------:|-----------:|--------:|--------:|
| CircuitEval | seq    |     0.619 |      1.21 |      2.00 |       2.02 |    1.62 |    2.21 |
| TwoPlusTwo  | seq    |     2.42  |      2.43 |      5.67 |       5.69 |    1.38 |    1.38 |
| OMPEval     | seq    |     1.40  |      1.41 |      2.40 |       2.40 |    1.35 |    1.35 |
| PHE         | seq    |     8.86  |      8.66 |     15.1  |      15.0  |    4.38 |    4.36 |
| CircuitEval | random |     0.620 |      1.22 |      2.00 |       2.03 |    1.62 |    2.21 |
| TwoPlusTwo  | random |    11.8   |     12.4  |     36.4  |      36.8  |    8.22 |    8.20 |
| OMPEval     | random |     1.67  |      1.68 |      3.53 |       3.52 |    1.59 |    1.59 |
| PHE         | random |    16.4   |     16.0  |     20.3  |      20.2  |   11.0  |   11.0  |

CircuitEval makes no data-dependent memory access, so its random and
sequential rows agree within 0.4% everywhere. Halving the lane count on
the same machine costs 1.96x. This benchmark really undersells how fast
TwoPlusTwo is at sequential enumeration. See the enumeration bench below
for a fair comparison.

## Main battery, omaha (bench/omaha.cc)

Same methodology; a deal is 5 board + k hole card ids, k = 4, 5, 6. The
hi-lo row runs the high and low circuits off one shared packed input.
PHE has no omaha hi-lo evaluator. ns/hand:

| Evaluator              | Order  | Zen 5 512 | Zen 5 256 | Zen 3 256 | Zen 3 port | M5 NEON | M5 port |
|------------------------|--------|----------:|----------:|----------:|-----------:|--------:|--------:|
| CircuitEval PLO4       | seq    |      1.04 |      2.42 |      3.35 |       3.34 |    2.70 |    3.59 |
| CircuitEval PLO5       | seq    |      1.06 |      2.42 |      3.36 |       3.41 |    2.78 |    3.71 |
| CircuitEval PLO6       | seq    |      1.08 |      2.51 |      3.43 |       3.48 |    3.03 |    3.82 |
| CircuitEval PLO4 hi-lo | seq    |      1.13 |      2.70 |      3.56 |       3.57 |    3.07 |    3.79 |
| PHE PLO4               | seq    |     11.9  |     13.5  |     19.5  |      19.4  |    8.52 |    8.09 |
| PHE PLO5               | seq    |     12.4  |     14.1  |     21.0  |      21.2  |    8.79 |    8.80 |
| PHE PLO6               | seq    |     13.6  |     14.2  |     23.7  |      23.5  |    9.97 |    9.57 |
| CircuitEval PLO4       | random |      1.03 |      2.42 |      3.35 |       3.34 |    2.83 |    3.59 |
| CircuitEval PLO5       | random |      1.06 |      2.42 |      3.36 |       3.41 |    2.79 |    3.71 |
| CircuitEval PLO6       | random |      1.08 |      2.51 |      3.43 |       3.48 |    2.87 |    3.83 |
| CircuitEval PLO4 hi-lo | random |      1.11 |      2.70 |      3.57 |       3.57 |    2.85 |    3.79 |
| PHE PLO4               | random |     38.7  |     39.9  |     56.5  |      56.6  |   26.3  |   26.1  |
| PHE PLO5               | random |     81.2  |     77.5  |     94.1  |      96.2  |   36.6  |   36.8  |
| PHE PLO6               | random |    124    |    123    |    139    |     134    |   45.1  |   44.9  |

One circuit serves all three k, so the CircuitEval rows differ only in
mask building: PLO5 and PLO6 hand more cards to the hole-mask call than
PLO4 does. The low circuit costs +0.08 to +0.28 ns/hand over the PLO4
row on the EC2 boxes.

## Best-implementation, enumeration order (bench/holdem_enum.cc, bench/omaha_enum.cc)

Each evaluator written to exploit the shared prefixes of nested-loop
enumeration, over its first 2^20 hands. TwoPlusTwo hoists prefix lookups
per loop level, OMPEval keeps one partial Hand per level, CircuitEval
carries the prefix's presence mask per level and ORs in the last two
cards from a table of two-card masks, one library call per full batch.
PHE keeps no per-prefix state, so it runs the plain loop.

The omaha variant enumerates 9-card deals the same way; the first 2^20
deals share one hole combination, so it is a fixed-hole board sweep.
ns/hand:

| Evaluator          | Zen 5 512 | Zen 5 256 | Zen 3 256 | Zen 3 port | M5 NEON | M5 port |
|--------------------|----------:|----------:|----------:|-----------:|--------:|--------:|
| CircuitEval holdem |     0.488 |     0.945 |     1.55  |      1.62  |   1.19  |   1.64  |
| TwoPlusTwo         |     0.247 |     0.224 |     0.404 |      0.405 |   0.429 |   0.472 |
| OMPEval            |     0.746 |     0.688 |     0.988 |      0.988 |   0.523 |   0.546 |
| PHE holdem         |     9.85  |     9.72  |    18.6   |     18.4   |   6.50  |   7.02  |
| CircuitEval PLO4   |     0.851 |     1.99  |     2.69  |      2.71  |   2.30  |   3.14  |
| PHE PLO4           |    14.0   |    13.9   |    23.2   |     23.6   |  10.1   |  11.4   |

For the other evaluators, the differences between the two Zen 5 columns
(up to 10% on TwoPlusTwo, 8.5% on OMPEval) are scalar codegen under
-march=native vs x86-64-v3, not vector width.

## Stability

Coefficient of variation over the 25 repetitions. Our rows: at or under
0.75% on the c8a, 0.06% on the c6a; on the mac 0.2% in the holdem
battery, 1.9-2.6% in enumeration, and 3.2-5.4% in the omaha battery. The
big-table random rows are the least stable everywhere but stay in single
digits: TwoPlusTwo random 0.9-4.4% and PHE omaha random 1.3-5.4% across
the three machines.

## Footprint (bench/sizes.sh)

Evaluation code (.text) and tables (.rodata/.data/.bss, plus
TwoPlusTwo's runtime-loaded file) per evaluator, in KiB and MiB. Our
sizes depend on the batch width and conversion core, so all four Linux
builds are shown; the others vary under 2% across them and are quoted
from the Zen 5 512 build. `sizes.sh` is Linux-only, so there are no mac
numbers.

| CircuitEval, code / data (KiB) | holdem     | omaha      | omaha hi-lo |
|--------------------------------|-----------:|-----------:|------------:|
| Zen 5 512                      | 19.9 / 1.1 | 30.1 / 0.9 |  31.4 / 1.0 |
| Zen 5 256                      | 22.6 / 0.5 | 32.5 / 0.3 |  34.1 / 0.3 |
| Zen 3 256                      | 22.6 / 0.5 | 32.5 / 0.3 |  34.1 / 0.3 |
| Zen 3 port                     | 23.0 / 0.5 | 32.9 / 0.4 |  34.4 / 0.4 |

The holdem and omaha columns are the circuit plus the conversion layer;
omaha hi-lo adds the low circuit. The portable conversion core is 342
bytes of code and 64 bytes of data larger than the native one.

| Evaluator  | Code (KiB) | Data (MiB) |
|------------|-----------:|-----------:|
| TwoPlusTwo |         ~0 |      123.9 |
| OMPEval    |       14.0 |        0.2 |
| PHE        |        0.9 |        0.1 |
| PHE PLO4   |        1.1 |       29.3 |
| PHE PLO5   |        1.1 |      109.1 |
| PHE PLO6   |        1.0 |      344.7 |

The PHE omaha rows also call ~1 KB of helpers counted under the PHE row.
