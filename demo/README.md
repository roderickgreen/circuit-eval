# demo/ -- WebGPU exact hand equity demo

The circuit evaluators running live in the browser: React 19 + TypeScript,
bundled with Vite, using WGSL shaders generated from the same circuits as 
the C code.

## Run it

```
npm install
npm run dev           # http://localhost:8000
npm run build         # typecheck + bundle into dist/
npm run preview       # serve dist/ on the same address
npm run typecheck     # tsc --noEmit, app + tests + vite config
npm test              # vitest run -- the app-logic tests (no GPU)
```

WebGPU needs a secure context.

## Layout

```
index.html            Vite entry
src/main.tsx          React root
src/App.tsx           table + settings state, and the handlers over them
src/components/       GpuBanner, Controls, Felt, Card, Picker, EquityPanel
src/hooks/            useEquityEngine (a subscription), usePrewarm,
                      useSpotUrl (the URL <-> felt binding)
src/lib/              everything that isn't React: GPU, kernels,
                      combinatorics, card model, table edit rules,
                      formatting, and engine.ts -- the run control
src/kernels/          generated WGSL, imported as assets and fetched at
                      runtime. Generated, validated and installed from
                      ../flow/codegen/webgpu (nothing in demo/ generates)
test/                 app-logic tests -- vitest on node, no GPU
```

`src/lib` is DOM-free and React-free; the validation harnesses in
`../verify/webgpu/` import it as-is, so what gets vetted
is the code the page runs.

## Pieces

- `src/kernels/*.wgsl` -- generated from `../flow/netlists/*.blif` by
  `../flow/codegen/webgpu`: `make kernels` stages into its
  `build/kernels/`, `make validate-kernels` vets the staged set, `make
  install` copies it here -- in that order, so nothing lands unvalidated.
  Committed so the demo needs no build step; regenerate only when a
  circuit changes.
- `src/lib/gpu.ts` -- adapter/device, pipeline compile with error scopes,
  kernel loading, and the mapped-readback fence. Kernels are
  `import.meta.glob`'d as `?url` assets: each build emits them
  content-hashed, safe to serve immutable. Off the page there is no
  bundler; harnesses install a disk reader via `setKernelReader`.
- `src/lib/equity.ts` + `src/lib/equity/` + `src/kernels/equity_*.wgsl` --
  exact equity by in-shader enumeration of every remaining runout.
  `equity.ts` is the API; under `equity/`, `kernels.ts` routes and compiles,
  `uniform.ts` lays out the uniform block, `dispatch.ts` submits chunks and
  reads tallies back, `schedules.ts` holds the grid walks. Uniforms carry known-card
  masks in plane space; a storage buffer carries the work lists
  (`src/lib/worklist.ts`, enumerated on the CPU); the readback is a small
  tally. Three formulations: deal-major (lanes = boards), board-major
  (`*_bm.wgsl`: one invocation per board, villain combos in lanes), routed
  by board count; and cross-compare (`*_xc.wgsl`) when both holes are
  part-unknown -- one workgroup per (board, hero-tile) cell evaluates each
  candidate hand once and compares scalar-vs-plane instead of running the
  circuit per pairing. Part-unknown hero draws loop on the CPU.
- `src/components/` + `src/lib/engine.ts` -- heads-up table with per-slot
  known/unknown state and a picker. `engine.ts` owns the run control (what
  starts by itself, what waits for a click, what is refused, pause / stop /
  supersede) as a plain class; `useEquityEngine` is a
  `useSyncExternalStore` over it, so run-control behavior is testable
  without a renderer. Card model, table edit rules, spot, and tally
  orientation are plain functions in `src/lib`. Settings persist in
  localStorage (`pokerEvalSettings.v1`).
- `src/lib/share.ts` -- a spot in a URL: the `#s=` fragment is a bit-packed
  base64url payload (variant/hole-count/hi-lo header, six bits per slot).
  The address bar tracks the felt both ways (`useSpotUrl`), so copying the
  URL is how a spot travels. It carries cards, not results -- the recipient's
  GPU recomputes.
- `../verify/webgpu/` -- naive from-the-rules reference
  evaluators and the GPU-vs-JS exact-tally validation. Run from
  `flow/codegen/webgpu/`; `--full` adds preflop-scale spots.
  `bench.ts` there is the value-kernel throughput harness
  (`perf_probe.js --bench` prints rows mirroring `bench/holdem.cc` /
  `bench/omaha.cc`: same pools, bit-identical LCG, checked by
  `bench_pool_test.js` and `bench_eval_test.js`). Deno's default adapter
  may be llvmpipe; pin a real GPU's ICD, e.g.:

  ```
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
    deno run --allow-read --unstable-webgpu validate/equity_test.js
  ```

- `test/` -- engine seating policy and table edit rules, on vitest's node
  environment (needed: half the engine cases assert behavior when
  `navigator.gpu` is absent, which node provides).

Equity needs no rank-scale conversion: values are order-isomorphic
(`docs/ENCODING.md`), so win/tie/loss is integer comparison. Omaha hole counts
2-8 come free from the same circuit, but only 4/5/6 have been through the
verification sequence.
