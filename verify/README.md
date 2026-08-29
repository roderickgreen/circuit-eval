# verify/ -- the gates

Every harness here checks a circuit, a netlist, or the pack layer against
an oracle that shares no code with it.  `Makefile` builds the compiled
harnesses and the oracle objects into `build/`; the Python harnesses run
from the repo root.  `make verify` in `flow/` runs the sequence that
every spec change goes through.

## Running the gates yourself

From the repo root; the oracles are submodules, the harnesses build with
`-march=native`.  Wall times are for a 32-thread machine.

```sh
git submodule update --init third-party/PokerHandEvaluator third-party/OMPEval
make -C verify
cd verify

# holdem: every 5-, 6- and 7-card hand
build/verify_nk 7                 # all C(52,7) hands ordinal-exact vs PHE, ~14 s
build/omporacle5                  # circuit-free 5-card stamp from OMPEval + the encoding
build/verify_holdem -k 5          # the library's 5-card stamp: must equal omporacle5's
build/verify_holdem -k 6          # 6-card vs 5-card, 0 mismatches -- PASS
build/verify_holdem -k 7          # 7-card vs 6-card, 0 mismatches -- PASS, ~0.3 s

# omaha high and low, per hole count (-k 2..6 are the validated counts)
build/verify_omaha -k 4 -p 0:200      # 201 hole sets x every board, ~0.2 s
build/verify_omaha_lo -k 4 -p 0:200   # the low, same slice
build/verify_omaha_hilo -k 4 -p 0:200 # both sides in one sweep, same stamps
build/verify_omaha -k 4               # the whole domain: 4.6e11 configs, ~3.5 min
build/pheoracle_omaha -k 4 -p 0:200   # circuit-free stamp from PHE + the encoding:
                                      # must equal verify_omaha's line for the slice
build/verify_omaha_hilo -k 4 -b 3     # 3-card boards (a flop), whole domain, both sides
build/verify_omaha_hilo -k 4 -b 4     # 4-card boards (a turn)
```

`-b` (3, 4 or 5, default 5) is the board size on the three omaha gates;
each board size is its own domain with its own stamps.

Each compiled gate prints `0 mismatches -- PASS` and exits 0, or names
the failure and exits 1.  The final line of every run is a stamp pair:
`stamp` folds a mixing hash over every configuration and its value,
`domain` over the configurations alone, so two runs of the same slice
must print the same pair, and a `-p first:last` slice is checkable on
its own.  Both stamps are sums mod 2^64 of one term per configuration,
so disjoint slices recombine into the full domain: their `domain` stamps
must sum to the constant for that k and b (`build/domain_omaha -k K
-b B`, or any gate's `-d`), then their `stamp` values sum to the
full-domain stamp.  The full omaha sweeps at 5 and 6
hole cards run for hours; `-p` slices give the same evidence per
configuration in seconds.

## Published stamps

The stamps the committed circuits produce.  A run on any machine,
backend, or batch width that covers the same configurations must print
the same pair.

| holdem `verify_holdem -k K` | configurations | stamp | domain |
|---|---:|---|---|
| k 5 (= `omporacle5`) | C(52,5) = 2,598,960 | `5acb911ca833864e` | `58d97a9f36ec547a` |
| k 6 | C(52,6) = 20,358,520 | `a0378392f83a77bc` | `cd9199a2a34e5f28` |
| k 7 | C(52,7) = 133,784,560 | `6ece768d07abf380` | `4ceed8f0415f9495` |

| omaha `-k K` | configurations C(52,k) x C(52-k,5) | high stamp | low stamp | domain |
|---|---:|---|---|---|
| k 2 | 2,809,475,760 | `f1cdef0e29e17931` | `2bad791b6eb4f5bd` | `00fe461d500b5a3c` |
| k 3 | 42,142,136,400 | `c742cbb43d65ce07` | `bcfe1ced25b3fa95` | `7706cf43817d556f` |
| k 4 | 463,563,500,400 | `d56c60a6f0f9cf9c` | `7ac0a5acf8812e9e` | `5069747267203a39` |
| k 5 | 3,986,646,103,440 | `aa8defedf2c6ce7c` | `f6ca63d20cfec8e2` | `74f0f49a6d46d7e8` |
| k 6 | 27,906,522,724,080 | `3a03655bd1cf8774` | `07465ff789a64301` | `4dadbe86d7f4cb80` |
| k 7 | 163,452,490,241,040 | | | `bf51ca0ef564be29` |
| k 8 | 817,262,451,205,200 | | | `3dc6e0ada922b323` |

| omaha `-k K -b B` | configurations C(52,k) x C(52-k,b) | high stamp | low stamp | domain |
|---|---:|---|---|---|
| k 2 b 3 | 25,989,600 | `68b88269a8129863` | `82441d4ae4073be3` | `f9edbb4900a7fe23` |
| k 2 b 4 | 305,377,800 | `e639f80cadb233ab` | `083699abe4826a09` | `3208594f9904c699` |
| k 3 b 3 | 407,170,400 | `f6714880abf93850` | `26fe1e79c7e4e299` | `a643fd71a3b6e65d` |
| k 3 b 4 | 4,682,459,600 | `d66f53390eae981b` | `5482295316046d68` | `909209ae060dcd3d` |
| k 4 b 3 | 4,682,459,600 | `c6596d2940eb97f7` | `9f71a96897b2e432` | `be6f49d57b5e0299` |
| k 4 b 4 | 52,677,670,500 | `2c228cb4fc776711` | `afab955606962d36` | `045ea6cc21dbfec6` |
| k 5 b 3 | 42,142,136,400 | `9bd04504eea919ac` | `2a06c0bf7aad0690` | `9da941bd1c86d9ae` |
| k 5 b 4 | 463,563,500,400 | `efa9085d97db847e` | `170c4fa6f3ab1d93` | `4716931110132610` |
| k 6 b 3 | 309,042,333,600 | `f9e640cea7af4f55` | `89f8f4ad9f68e49b` | `39607d97158fe565` |
| k 6 b 4 | 3,322,205,086,200 | `dacc8a469fcf3437` | `136676bd77bd0de3` | `d2f956d7e42e6cb1` |

The 3- and 4-card board rows (a flop, a turn) come from
`verify_omaha_hilo -k K -b B`; `verify_omaha -b` and `verify_omaha_lo -b`
print the same stamps per side.  They are held to the anchored 5-card
primitive and the rules-only low the same way the 5-card board rows
are; `pheoracle_omaha` takes 5-card boards only, so the circuit-free
stamp exists for the b 5 rows alone.  Every row above through k 6 b 4
ran on one 32-thread machine, the largest in about an hour.

`verify_omaha_hilo` prints both value stamps and the shared domain on
one line; each equals what the matching single-side gate prints for the
same slice.  `pheoracle_omaha -k K` (K = 4, 5, 6) prints the high stamp
from PokerHandEvaluator's plo evaluators and the encoding table, with no
circuit code; it reproduces the k 4 row above in full (about 12 min on
32 threads) and any `-p` slice of k 5 and k 6.  The omaha domain is
shared by the high and low runs at a hole count.  k 7 and k 8 have
domain constants only: no value sweep has been run at those hole counts,
so the circuits are not validated there.  The k 5 and k 6 value runs
took 3-30 min each on 96 cores.

## What is here

### `holdem/` -- the holdem value circuit

`verify_holdem.sh` is the exhaustive gate: the candidate's per-class
values diffed bit-for-bit against the encoding table (`dump_valmap` vs
`flow/encoding/mkspec.py`), then all C(52,5)+C(52,6)+C(52,7) hands
against PHE for ordinal isomorphism (`verify_nk`).  `verify_holdem.cpp`
holds the deployed library to `omporacle5`, a stamp oracle built from
OMPEval and the encoding spec with no circuit code.  `crosscheck`
checks the two oracles against each other.  `fullsweep` validates the
reduced-domain tables `artifacts/mkartifacts.py` writes.
`verify_cmp.py` proves the comparators exact over sampled pairs.

### `omaha/` -- the omaha circuits

`verify_omaha.c` holds the library to the anchored 5-card values over
every configuration per hole count and board size (`-b 3`, `4`, `5`:
the expected value is the max over the board's C(b,3) triples).
`pheoracle_omaha.c` is the
circuit-free counterpart at 4-6 hole cards: PHE's omaha evaluators
aligned to the encoding the way `omporacle5` aligns OMPEval, printing
the same stamp line.  `verify_omaha_lo.c` does the same for the low
against an in-process rules-only reference.
`verify_omaha_hilo.c` builds both references over one board
enumeration and tests `circuit_eval_omaha_hilo`, which runs the two
circuits off one input transpose; it validates both sides for about 17%
less than the two single-side sweeps cost separately.
`omaha_sweep.h` is the scaffolding the three gates and `domain_omaha.c` share:
the card tables, colex hole-set indexing, the stamp primitive, the
command line, and the thread pool; it reads no evaluator and no
reference.  `verify_omaha.py` / `verify_low.py` are the sampled netlist
checks against the structural prototype `omaha_proto.py`.  `compose_low.py`
composes the low core onto the 104 card planes.

### `spec/` -- netlist-level checks

`bitsim.py` is a bit-parallel BLIF simulator.  `verify_value.py` checks
the rank and flush cores against the ground-truth tables.
`crosscheck.py` is the sampled spot check against the committed holdem
netlist for the spec edit loop.  `mkvalmap.py` writes the dense class
id -> 24-bit value table the holdem gate diffs against.

### `pack/` -- the pack/unpack layer in `lib/bspack/`

Correctness plus per-phase timing at either plane width
(`make -C verify/pack`).

### `webgpu/` -- the WGSL kernels

Checked against a from-the-rules JavaScript reference, run headless
through the demo's own GPU code
(`make -C flow/codegen/webgpu validate-kernels`).

### `artifacts/` -- ground truth

`mkartifacts.py` writes the reduced-domain ground truth the spec checks
read (to `../artifacts/`, gitignored).
