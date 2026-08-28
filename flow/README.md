# flow/ -- from Verilog spec to checked-in C

The pipeline that produced the library, one directory per stage, in
order:

- `spec/` -- the behavioral Verilog for each circuit (`rank_value`,
  `flush_value`, `holdem_value`, `suit_view`, `omaha_value`, `low_core`,
  `cmp8`, `cmp24`) and the shared `lib.vh`.
- `Makefile` -- elaborates each spec with yosys to an AIG, normalizes it
  with ABC to 2-input BLIF (`make blifs`), and runs the verification
  sequence in `../verify` against the result (`make verify`).  Output
  lands in `build/`.  The root Makefile forwards these targets.
- `synth/` -- the long-run minimization: `restartloop.sh` runs
  independent `&deepsyn` lotteries from the normalized starts,
  `restartstatus.sh` and `deepsynwatch.sh` report on a run,
  `mkwrappers.py` writes the IO wrappers the Makefile elaborates,
  `synth.sh` wraps `make verify`.
- `netlists/` -- the winning BLIFs and their pre-polish AIGs, with the
  provenance table and the recipe that identifies each run
  (`netlists/README.md`).
- `codegen/` -- backends from BLIF: `cpu/bitslice.py` writes the
  library's `lib/generated/circuit_*.c` (`make -C lib regen`);
  `cpu/bitslice64.py` is the readable uint64 variant `teach/` uses;
  `webgpu/` writes the WGSL kernels `demo/` runs (`make -C
  codegen/webgpu install`); `blif.py` is the shared front end (parse,
  gate scheduling, expression rendering); `orderio.py` / `renameio.py`
  fix netlist input order and PI/PO names.
- `encoding/` -- `mkspec.py`, the value-encoding table generator; the
  encoding it implements is `../docs/ENCODING.md`.

## Long synthesis runs

The node counts in `netlists/` come from long stochastic `&deepsyn` runs
on top of the normalized starts `make blifs` builds:

```sh
J=1000 PAIRS=4 SEED0=5000 ARMS=t bash flow/synth/restartloop.sh
```

`restartloop.sh` runs independent `&deepsyn -T 0 -J <J> -S <seed>`
lotteries from the start netlist. Each self-terminates once it stalls
(J steps without improvement) and the next seed begins. Deepsyn is
not bit-reproducible even at a fixed (start, seed, J) -- see
`netlists/README.md` -- so each lane keeps every completed draw.
Knobs (lanes, seeds, arms, start netlists) are documented in the script
header. Size the parallelism by RAM, not cores.
