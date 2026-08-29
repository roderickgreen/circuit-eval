# circuit-eval: Fast poker hand evaluation via hardware synthesis and bitslicing

Batch poker hand evaluators for holdem and omaha (PLO4, PLO5, PLO6, and
omaha hi-lo), synthesized as combinational circuits and compiled to C.
Each call evaluates 128-512 hands in parallel. No lookup tables, so
random deals run as fast as sequential ones.

## References

[Blog post](https://roderickgreen.com/posts/fast-poker-hand-eval/)\
[WebGPU Demo](https://roderickgreen.com/demos/circuit-equity/)

## Expectations

This code is provided for free as part of a project I did with Claude
for fun. I can't promise support for any of this work. If you find a
correctness bug in the circuits or generated code, please reach out.

## Quick start

The evaluators are pre-generated C in `lib/`. GCC or Clang is the only
dependency (the generated code uses GNU vector extensions):

```sh
make        # lib/build/libcircuiteval.a
make test   # selftest: known deals in, exact expected values out
make bench  # vs TwoPlusTwo / OMPEval / PHEvaluator (needs submodules + cmake)
```

`lib/` and `include/` are the whole library. Nothing else in the repo is
needed to build or link it. The API (`include/circuit_eval.h`) is
batch-only: every call evaluates `BS_BATCH` hands, a compile-time width
(`include/bsword.h`) that follows the target's vector register: 512 when
compiled for AVX-512, 128 on NEON, 256 elsewhere. One gate is one vector
op at the machine's full width.

```c
#include <stdio.h>
#include "circuit_eval.h"

int main(void)
{
    uint64_t hands[BS_BATCH] = {0};  /* presence masks: bit (4*rank+suit) per card;
                                        rank 0 = deuce .. 12 = ace, suits are
                                        interchangeable labels 0..3 (here s h d c) */
    uint32_t vals[BS_BATCH];         /* 24-bit order-isomorphic values: bigger wins */
    hands[0] = 1ull << 48 | 1ull << 44 | 1ull << 40 | 1ull << 36 |
               1ull << 32 | 1ull << 9 | 1ull << 2;    /* As Ks Qs Js Ts 4h 2d */
    circuit_eval_holdem(hands, vals);
    printf("category %d\n", CIRCUIT_CATEGORY(vals[0]));   /* 9: straight flush */
    return 0;
}
```

```sh
cc -O2 -march=native -I include main.c lib/build/libcircuiteval.a
```

`circuit_eval_omaha` and `circuit_eval_omaha_hilo` take hole and board
masks separately. One circuit serves arbitrary hole card counts, and
hi-lo adds the 8-or-better low. An omaha hand is the best five cards
formed from exactly two hole cards and exactly three board cards. The
low is the best such five with distinct ranks all eight or lower, ace
counting as low and straights and flushes ignored (`docs/ENCODING.md`
gives both value encodings). Fused `circuit_equity_*` calls count
heads-up showdowns without leaving plane space. `examples/` holds
complete downstream programs (holdem and omaha showdown, heads-up equity
through the fused call and again at the plane level, a 7-card category
census) and their Makefile. `make examples` builds them beside their
sources.

Link `lib/build/libcircuiteval.a` with `-I include`, or compile the `.c`
files under `lib/` directly. Every object in a binary must be compiled
at the same `BS_BATCH`. A link error `undefined reference to
bs_batch_width_N` means an object was compiled at width N and the
library at another -- rebuild both with the same `-march` (and
`-DBS_BATCH`, if set). Because `BS_BATCH` follows `-march`, a library
built on an AVX-512 host does not run on an AVX2 one: for binaries that
ship, build with `ARCH=` pinned to the deployment baseline (for example
`ARCH=x86-64-v3`). A `bs_word` must be aligned to its own size
(`BS_ALIGN`, 32 or 64 bytes); `malloc` aligns to 16, so plane buffers on
the heap come from `aligned_alloc(BS_ALIGN, size)` or an aligned vector
store faults. Encodings and the plane-level interface are documented in
`include/circuit_eval.h` and `include/bsapi.h`.

### Input contract

Inputs are not validated. Each circuit is a total function on its input
bits: any pattern produces a well-formed value, but only a legal deal
produces a meaningful one.

| circuit | input contract |
|---|---|
| holdem value | 5, 6 or 7 of the 52 bits set |
| omaha value | >= 2 hole bits, >= 3 board bits, hole and board disjoint |
| composed low | same as omaha value |

Anything else yields a well-formed but meaningless value, not an error
or a sentinel. Any required validation must be done upstream.

The omaha circuits work for any hole count of 2 or more and any board
of 3 to 5 cards. They are verified exhaustively for 2 through 6 hole
cards on 3-, 4- and 5-card boards (`verify/build/verify_omaha -k K -b B`,
see `verify/README.md`).

## The evaluators

Each evaluator is a combinational circuit: a behavioral Verilog spec
(`flow/spec/`), synthesized to an AIG with yosys, minimized to 2-input
gates with ABC (the node-count minimization runs under `&deepsyn`),
compiled to bitsliced C with one statement per gate, 128-512 hands per
call.

The verified netlists live in `flow/netlists/`, node count in each
filename. The per-file recipe and verification record is
`flow/netlists/README.md`.

| circuit | computes | nodes |
|---|---|---|
| `holdem_value_1192` | 7-card Hold'em -> 24-bit order-isomorphic value; exact at 5 and 6 cards too | 1,192 |
| `omaha_high_2324` | Omaha high; one netlist exact for PLO4/5/6 | 2,324 |
| `low_core_133` + composed | Omaha 8-or-better low; also hole-count-free | 181 composed |
| `cmp24_107` / `cmp8_35` | value comparators: per-lane gt/eq (win/tie/loss) for fused equity loops | 107 / 35 |

Values compare as plain integers (`docs/ENCODING.md`). Head-to-head
methodology and results vs TwoPlusTwo / OMPEval / PHEvaluator, plus the
code/data footprint table: `docs/BENCH.md` (`make bench` reproduces it).

Verification is exhaustive where the domain allows:
- all C(52,7) seven-card hands ordinal-exact against PHE plus a
  bit-exact per-class encoding-contract diff
- every Omaha configuration (hole set x board) for 2 through 6 hole
  cards, high and low, requiring 64.8 trillion total evals and 100 core
  hours
- all 54,093 realizable rank-set pairs for the low circuit in isolation
- million-deal ordinal omaha crosschecks against OMPEval for smoke
  testing

The commands to run every gate are in `verify/README.md`.

Two other backends: `flow/codegen/webgpu` compiles the same netlists to
WGSL compute shaders and `teach/popcnt4` runs the identical pipeline on
a 9-gate circuit you can read end to end.

### Codegen choices

The generated circuit C is one statement per gate on a GNU vector type.
The batch width follows the compile target: 512 hands per call on
AVX-512 hosts, 128 on NEON, 256 elsewhere (`BS_BATCH` pins it, down to
plain-`uint64_t` 64 for machines without wide SIMD).

The conversion layer around the circuit (`lib/bspack/`) is where
per-target code lives. Its kernels have hand-scheduled cores selected at
compile time by `bspack.c`:
- pack/unpack (masks <-> bit planes): fused intrinsic schedules for
  AVX-512 (GFNI/VBMI), AVX2, and arm64/NEON. Every other target gets a
  portable composition built on a generic 64x64 transpose in plain
  vector ops.
- cards -> masks (`bs_masks_from_cards`): vector cores for the same
  three targets, plus a scalar core that is always compiled (it is the
  runtime fallback for shapes the vector cores refuse).

Two build knobs pin a generic core where a specialized one exists,
without touching `-march`, so the gain from each hand-written schedule
can be priced on any machine (`make -C bench run-bspack` with the knob
times it against the default build):

- `PACK=portable` -- the portable pack/unpack composition instead of the
  fused intrinsics.
- `MASKS=scalar` -- the scalar masks core instead of the vector one.

`BATCH=` and `ARCH=` pin the layout width and the instruction set. All
four knobs work in both `lib/` and `bench/`. Different knob values
cannot share a build directory (`make clean` between them).

## Repo layout

### The library

- `include/` -- the API: `circuit_eval.h` (card masks in, values out),
  `bsapi.h` (the plane-level layer), `bsword.h` (`BS_BATCH`)
- `lib/` -- the API implementation, the transpose front end (`bspack/`),
  and the generated circuit C (`generated/`; `make -C lib regen`
  rebuilds it from `flow/netlists/`)
- `examples/` -- downstream programs built against the library
- `docs/` -- `ENCODING.md` (the value encoding), `BENCH.md` (results and
  methodology)

### Synthesis and codegen

- `flow/` -- in order: `spec/` (Verilog), `synth/` (the
  yosys/ABC/deepsyn scripts, driven by `flow/Makefile`), `netlists/`
  (the verified BLIFs and their provenance), `codegen/` (BLIF to
  bitsliced C and to WGSL), `encoding/` (the value-table generator). See
  `flow/README.md`.
- `teach/` -- the same pipeline end to end on a 9-gate circuit

### Validation

- `test/` -- the selftest `make test` runs
- `verify/` -- the verification harnesses and their oracles, by what
  they verify (`holdem/`, `omaha/`, `spec/`, `pack/`, `webgpu/`). See
  `verify/README.md`.
- `bench/` -- publication benchmarks (google benchmark)

### Demo

- `demo/` -- browser equity calculator using the WGSL kernels

### `third-party/` (submodules, built in place)

- `abc` -- synthesis, built in place
- `yosys` -- Verilog elaboration, vendored at v0.68, built in place
- `PokerHandEvaluator`, `OMPEval`, `TwoPlusTwoHandEvaluator` -- alternate
  evaluators used as oracles and benchmark comparisons
- `benchmark` -- google-benchmark, for `bench/`

## Benchmarking

```sh
make bench                            # results + methodology in docs/BENCH.md
make bench PERF=1                     # the same, plus CYCLES/hand and INSTRUCTIONS/hand
make -C bench run-bspack              # the pack/unpack layer alone
make -C verify/pack                   # per-phase pack/unpack timings
```

`make bench` runs 3 repetitions unpinned. The tables in `docs/BENCH.md`
come from `make -C bench report`: 25 repetitions, the process pinned to
one core, random interleaving, and the PMU columns where libpfm4 is
installed.

First `make bench` generates the 2+2 table and compiles PHE's plo5/plo6
table sources: ~10 min once per machine.

`PERF=1` needs `apt install libpfm4-dev` and `sudo sysctl -w
kernel.perf_event_paranoid=2`. Every run target takes it, and `report`
sets it itself where libpfm4 is installed.

## The value-encoding contract

The 24-bit encoding is defined in `docs/ENCODING.md`. `verify_holdem.sh`
enforces it on every holdem candidate against the table
`flow/encoding/mkspec.py` generates. By hand:

```sh
make -C verify build/dump_valmap
diff <(python3 flow/encoding/mkspec.py) <(./verify/build/dump_valmap)   # must be empty
```

The omaha circuit is held to the same encoding by
`verify/omaha/verify_omaha.c`: exhaustively per hole count, its value
must equal the max over 2-hole/3-board splits of the anchored 5-card
value.

## Building the toolchain (abc and yosys)

Only the synthesis workflow and `teach/` need these. `make toolchain`
fetches the two submodules and builds both in place. It runs the
following from the repo root:

```
git submodule update --init third-party/abc
git submodule update --init --recursive third-party/yosys

# abc -> third-party/abc/abc
make -C third-party/abc -j "$(nproc)" ABC_USE_NO_READLINE=1   # drop the flag for a readline shell

# yosys -> third-party/yosys/build/yosys
cmake -S third-party/yosys -B third-party/yosys/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DYOSYS_ABC_EXECUTABLE="$(pwd)/third-party/abc/abc"
cmake --build third-party/yosys/build --parallel "$(nproc)"
```

`YOSYS_ABC_EXECUTABLE` points yosys at the abc built above, so its build
does not compile the bundled abc copy. The synthesis Makefile calls
`third-party/abc/abc` directly for every ABC pass. The
`ABC_USE_NO_READLINE=1` flag drops abc's dependency on readline. Omit it
to get command history and line editing in abc's interactive shell,
which then needs `libreadline-dev` installed. yosys needs CMake >= 3.28,
GNU Flex, and GNU Bison >= 3.8. The `YOSYS` and `ABC` variables in
`flow/Makefile` default to these two paths. Override them to point at a
toolchain built elsewhere.

Development was done on Ubuntu 24.04.

## The synthesis workflow

Not needed to build or use the library. All commands run from the repo
root and need `third-party/abc` and `third-party/yosys` built, the
oracle submodules fetched, and the ground-truth artifacts generated
(written to `artifacts/`, gitignored):

```sh
git submodule update --init third-party/PokerHandEvaluator third-party/OMPEval
python3 verify/artifacts/mkartifacts.py                            # reduced-domain ground truth
python3 verify/spec/mkvalmap.py artifacts/factored/valmap.bin      # dense id -> 24-bit value
make -C verify build/fullsweep && verify/build/fullsweep           # exhaustive sweep vs PHE, must PASS
```

`flow/Makefile` elaborates each Verilog spec with yosys, normalizes to
2-input BLIF with ABC, and runs the verification sequence (the root
`Makefile` forwards these targets):

```sh
make check              # fast parse/elaborate of all specs (the edit loop)
make blifs -j           # build every AIG/BLIF that is out of date
make verify             # build + the full verification sequence
make verify-omaha       # one top: rank flush holdem low omaha cmp8 cmp24
```

What `make` produces is a starting point. The node counts in
`flow/netlists/` come from long stochastic `&deepsyn` runs on top of it
(`flow/synth/restartloop.sh`, see `flow/README.md`). Fresh starts vs the
committed netlists: holdem 1,509 -> 1,192, omaha 3,635 -> 2,324, low
core 471 -> 133, cmp24 119 -> 107, cmp8 38 -> 35.

### Promoting a new circuit

`flow/codegen/renameio.py` restores the PI/PO names deepsyn discards
(positionally, since ABC preserves order). Then the verification
sequence decides -- never node count alone:

```sh
sh verify/holdem/verify_holdem.sh cand.blif        # EXHAUSTIVE at nh=5/6/7 (~26 s)
for nh in 4 5 6; do python3 verify/omaha/verify_omaha.py cand.blif 20000 $nh; done
python3 verify/omaha/compose_low.py core.blif low.blif && python3 verify/omaha/verify_low.py low.blif 5000
```

`verify_holdem.sh` diffs the candidate's per-class values bit-for-bit
against the encoding table, then runs all C(52,5)+C(52,6)+C(52,7) =
156,742,040 hands against PHE for ordinal isomorphism. It builds the
candidate under its own prefix, so it is safe to run against the
committed circuit's builds.

To promote: add the BLIF to `flow/netlists/` (keep the old file), update
the table there, point `lib/Makefile`'s `regen` rule at it, then:

```sh
make -C lib regen && make && make test
```

Clean the harness builds after a swap (`make -C verify/pack clean`;
remove `circuit*.[co]` under `verify/build`): a new netlist is often
older than the generated C it replaces, and make compares mtimes. `lib/`
and `bench/` are immune -- their circuit C is the checked-in source
`regen` just rewrote.

## License

MIT (see `LICENSE`) for this repository's code. `third-party/`
submodules carry their own licenses.
