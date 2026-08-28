// Throughput benchmark for the generated hand-value circuits, over the
// demo's own GPU plumbing (demo/src/lib/gpu.ts).
//
// The question this answers is "do I evaluate hands on the CPU or move them
// to the GPU", so it prices the whole round trip rather than the circuit
// alone. It mirrors bench/holdem.cc and bench/omaha.cc: one workload shape,
// two hand orders, 2^20 hands per pool, one value per hand out.
//
//   Sequential   consecutive (7 | k+5)-card combinations in enumeration
//                order (colex, via Gosper's hack) from the first combination
//   Random       uniformly sampled hands, same fixed-seed LCG as the C
//                harness -- so the GPU chews the identical pool
//
// main_eval is the C front door's convention (include/circuit_eval.h): card
// masks in, one u32 value per hand out, with the mask <-> plane transposes
// that bspack.c does CPU-side -- and on the CPU harness's clock -- run
// in-shader around the circuit. So the two sides are priced on the same
// terms, which is the whole point of quoting ns/hand for both.
//
// On the clock: the whole job -- everything between "here is a pool of
// hands in host memory" and "every value sits in an ordinary preallocated
// array": the upload, the dispatch, the in-shader transposes, the circuit
// evals, the readback and the memcpy out of the mapped staging range. A
// pass moves the pool in CHUNK-hand slices through a ring of SLOTS slots,
// so one chunk's upload and another's readback overlap a third's compute
// instead of serializing behind a full-pool drain; what gets reported is
// whole passes, amortized. Off the clock: pool generation, shader
// compilation and pipeline creation. The C harness likewise builds its
// pools and warms its code outside the timed region -- though its pool is
// already in the memory the evaluator reads, a privilege the GPU round
// trip does not get: moving the hands is part of what is priced here.
//
// The C harness folds every value into an accumulator inside its timed
// region, to stop the compiler deleting the work. That device does not
// port: a JS reduction over 2^20 values costs many milliseconds -- more
// still if it reads the mapped range directly, which is uncached -- so
// folding on the clock would report mostly JavaScript. Nothing can be
// elided on this side anyway, since copyBufferToBuffer and mapAsync move
// the bytes whether or not anyone looks at them. So the fold moves off the
// clock, where it still runs before and after the timed loop as a checksum:
// a fixed pool must produce identical values every rep.
//
// These are the *value* circuits, the same ones the C benchmark runs. The
// equity kernels behind the main page are a different shape -- they
// enumerate in-kernel and read back only a tiny tally -- and are not
// measured here.

import { getGPU, makePipeline, parseMeta, adapterLabel, loadKernelSource } from "../../demo/src/lib/gpu.ts";

export interface BenchConfig {
  key: string;
  label: string;
  detail: string;
  kernel: string;
  /** hole-card count; 0 means hold'em, where one mask carries all seven cards */
  k: number;
}

export interface BenchRow {
  config: string;
  order: string;
  reps: number;
  checksum: number;
  nsPerHand: number;
  handsPerSec: number;
}

export interface BenchResults { adapter: string; rows: BenchRow[] }

/** what runBenchmarks reports through onProgress before a row lands */
export type BenchPhase = "pool" | "warm" | "run";

const NHANDS = 1 << 20;    // hands per pool, matching bench/*.cc
const WG = 64;             // workgroup_size in the generated kernels
const TARGET_MS = 400;     // timed-region budget per row
const MIN_REPS = 4;
const MAX_REPS = 512;

// CHUNK trades overlap against dispatch width: smaller chunks pipeline
// transfer and compute at a finer grain, but each dispatch shrinks to
// CHUNK/32/WG workgroups (which still has to fill the GPU) and the fixed
// per-chunk submit and map costs multiply. 2^17 is a compromise, not a
// law -- it is the knob to turn first on new hardware. SLOTS = 3 keeps
// one chunk uploading, one computing and one reading back at all times;
// deeper brings nothing while the three stages are the only stages.
export const CHUNK = 1 << 17;  // hands per in-flight slice of a pass
export const SLOTS = 3;        // slices in flight

// k is the hole-card count; 0 means hold'em, where one mask carries all
// seven cards. The omaha circuit serves k = 4, 5 and 6 exactly -- one
// kernel, three workloads, the same way the C harness uses one circuit.
export const CONFIGS: BenchConfig[] = [
  { key: "holdem", label: "Hold'em", detail: "7 cards", kernel: "holdem_value_l1", k: 0 },
  { key: "omaha4", label: "Omaha", detail: "4 hole + 5 board", kernel: "omaha_high_l1", k: 4 },
  { key: "omaha5", label: "Omaha", detail: "5 hole + 5 board", kernel: "omaha_high_l1", k: 5 },
  { key: "omaha6", label: "Omaha", detail: "6 hole + 5 board", kernel: "omaha_high_l1", k: 6 },
];

export const ORDERS = ["sequential", "random"];

// ---- pools ---------------------------------------------------------------
//
// Card space, not plane space: main_eval routes cards to circuit input
// planes itself. A mask is a vec2<u32> -- bit 4*rank+suit, .x holding ids
// 0..31 and .y ids 32..51 -- and the buffer is mask-array-major, so mask a
// of hand i sits at vec2 index a*NHANDS + i, i.e. words 2*(a*NHANDS + i)
// and +1. Hold'em uses one mask array; omaha uses two (hole, then board).

const maskWord = (a: number, i: number) => 2 * (a * NHANDS + i);

function setCard(pool: Uint32Array, o: number, id: number) {
  pool[o + (id >> 5)] |= 1 << (id & 31);
}

// Gosper's hack over 32-bit ints. Safe here because 2^20 combinations of 7
// (or of k+5, k <= 6) never reach past bit 27 -- the pools sit early in
// colex order -- so neither x + u nor the shift distance runs off the end.
function gosper(x: number): number {
  const u = x & -x;
  const v = (x + u) >>> 0;
  return (v | (((x ^ v) >>> 0) >>> (31 - Math.clz32(u) + 2))) >>> 0;
}

// The C harness's LCG (bench/holdem.cc, bench/omaha.cc,
// flow/codegen/cpu/bench.c): state = state * 6364136223846793005 +
// 1442695040888963407, draw = state >> 33. Held in 16-bit limbs so every
// partial product stays exact in a double and the random pools come out
// bit-identical to the CPU run's.
class LCG {
  s: number[];
  constructor() {
    this.s = [0x7c15, 0x7f4a, 0x79b9, 0x9e37]; // 0x9e3779b97f4a7c15
  }
  next() {
    const [s0, s1, s2, s3] = this.s;
    const m0 = 0x7f2d, m1 = 0x4c95, m2 = 0xf42d, m3 = 0x5851; // 0x5851f42d4c957f2d
    let p0 = s0 * m0 + 0x814f;                                // + 0x14057b7ef767814f
    let p1 = s0 * m1 + s1 * m0 + 0xf767;
    let p2 = s0 * m2 + s1 * m1 + s2 * m0 + 0x7b7e;
    let p3 = s0 * m3 + s1 * m2 + s2 * m1 + s3 * m0 + 0x1405;
    const r0 = p0 & 0xffff;
    p1 += Math.floor(p0 / 65536);
    const r1 = p1 & 0xffff;
    p2 += Math.floor(p1 / 65536);
    const r2 = p2 & 0xffff;
    p3 += Math.floor(p2 / 65536);
    const r3 = p3 & 0xffff;
    this.s = [r0, r1, r2, r3];
    return (r3 * 65536 + r2) >>> 1; // state >> 33
  }
}

// Partial Fisher-Yates over the 52-card deck, n cards drawn. The C code
// reinitialises the deck every hand; undoing the n swaps in reverse leaves
// it in the identical state for a fraction of the work.
function draw(rng: LCG, deck: Int32Array, undo: Int32Array, n: number) {
  for (let t = 0; t < n; t++) {
    const j = t + rng.next() % (52 - t);
    undo[t] = j;
    const tmp = deck[t]; deck[t] = deck[j]; deck[j] = tmp;
  }
}

function undraw(deck: Int32Array, undo: Int32Array, n: number) {
  for (let t = n - 1; t >= 0; t--) {
    const j = undo[t];
    const tmp = deck[t]; deck[t] = deck[j]; deck[j] = tmp;
  }
}

const newDeck = () => Int32Array.from({ length: 52 }, (_, i) => i);

// One 7-card mask per hand.
function holdemPool(order: string): Uint32Array<ArrayBuffer> {
  const pool = new Uint32Array(NHANDS * 2);
  if (order === "sequential") {
    let x = (1 << 7) - 1; // lowest 7-card combination
    for (let i = 0; i < NHANDS; i++) {
      const o = maskWord(0, i);
      for (let m = x; m; m &= m - 1) setCard(pool, o, 31 - Math.clz32(m & -m));
      x = gosper(x);
    }
  } else {
    const rng = new LCG(), deck = newDeck(), undo = new Int32Array(7);
    for (let i = 0; i < NHANDS; i++) {
      const o = maskWord(0, i);
      draw(rng, deck, undo, 7);
      for (let t = 0; t < 7; t++) setCard(pool, o, deck[t]);
      undraw(deck, undo, 7);
    }
  }
  return pool;
}

// Two mask arrays per pool: hole cards, then the 5-card board.
//
// The sequential split follows bench/omaha.cc's code: `hole = x; 5x hole &=
// hole - 1` clears the five *lowest* set bits, so the hole cards are the
// combination's top k and the board its bottom 5. (The comment above it in
// omaha.cc claims the opposite split -- see demo/README.md. The code is
// what produced the published CPU numbers, so the code is what is
// mirrored.)
function omahaPool(order: string, k: number): Uint32Array<ArrayBuffer> {
  const n = k + 5;
  const pool = new Uint32Array(2 * NHANDS * 2);
  if (order === "sequential") {
    let x = (1 << n) - 1;
    for (let i = 0; i < NHANDS; i++) {
      let hole = x;
      for (let t = 0; t < 5; t++) hole &= hole - 1; // drop the 5 lowest cards
      hole = hole >>> 0;
      const board = (x ^ hole) >>> 0;
      const oh = maskWord(0, i), ob = maskWord(1, i);
      for (let m = hole; m; m &= m - 1) setCard(pool, oh, 31 - Math.clz32(m & -m));
      for (let m = board; m; m &= m - 1) setCard(pool, ob, 31 - Math.clz32(m & -m));
      x = gosper(x);
    }
  } else {
    const rng = new LCG(), deck = newDeck(), undo = new Int32Array(n);
    for (let i = 0; i < NHANDS; i++) {
      const oh = maskWord(0, i), ob = maskWord(1, i);
      draw(rng, deck, undo, n);
      // the first k drawn are the hole cards, the last 5 the board
      for (let t = 0; t < k; t++) setCard(pool, oh, deck[t]);
      for (let t = k; t < n; t++) setCard(pool, ob, deck[t]);
      undraw(deck, undo, n);
    }
  }
  return pool;
}

// exported for bench_pool_test.js, which checks the pools against
// the C harness's generators before any of this is quoted next to their
// numbers
export const buildPool = (cfg: BenchConfig, order: string): Uint32Array<ArrayBuffer> =>
  cfg.k ? omahaPool(order, cfg.k) : holdemPool(order);

export const POOL_HANDS = NHANDS;
export { LCG, gosper };

// ---- running -------------------------------------------------------------

const srcCache = new Map<string, string>();
async function loadKernel(name: string): Promise<string> {
  let src = srcCache.get(name);
  if (src === undefined) {
    src = await loadKernelSource(name);
    srcCache.set(name, src);
  }
  return src;
}

// The machinery for moving pools through main_eval: SLOTS identical
// slots, each an input buffer, an output buffer, a staging buffer and a
// bind group, sized for one chunk. A pass rotates chunks through the
// slots; a slot is reused only once its previous chunk's values are out,
// which is also the proof that all three of its buffers are idle.
function makeSlots(device: GPUDevice, pipeline: GPUComputePipeline, nmask: number) {
  const bytes = nmask * CHUNK * 8; // one vec2<u32> per mask per hand
  const slots = Array.from({ length: SLOTS }, () => {
    const inBuf = device.createBuffer({
      size: bytes,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    const outBuf = device.createBuffer({
      size: CHUNK * 4,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
    });
    const staging = device.createBuffer({
      size: CHUNK * 4,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const bind = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: inBuf } },
        { binding: 1, resource: { buffer: outBuf } },
      ],
    });
    return { inBuf, outBuf, staging, bind, busy: null as Promise<void> | null };
  });
  const destroy = () => {
    for (const s of slots) {
      s.inBuf.destroy();
      s.outBuf.destroy();
      s.staging.destroy();
    }
  };
  return { slots, destroy };
}

// One pass: the whole pool up, through the circuit, and every value back
// into `dest`. Per chunk: upload the slice, submit dispatch + copy to
// staging, and start the readback; then move on. Queue order keeps a
// slot's upload behind its previous dispatch, and the await on `busy`
// keeps its staging unmapped -- and everything it owns idle -- before
// reuse. The awaits are the only stalls, and they only bite when the CPU
// gets a full ring ahead, which is the pipeline working as intended.
type Slot = ReturnType<typeof makeSlots>["slots"][number];

async function onePass(device: GPUDevice, pipeline: GPUComputePipeline,
                       groups: number, nmask: number, slots: Slot[],
                       pool: Uint32Array<ArrayBuffer>, dest: Uint32Array) {
  for (let c = 0; c * CHUNK < NHANDS; c++) {
    const s = slots[c % SLOTS];
    if (s.busy) await s.busy;
    // the pool is mask-array-major over NHANDS and the slot over CHUNK,
    // so each mask array's slice is its own copy
    for (let a = 0; a < nmask; a++) {
      device.queue.writeBuffer(s.inBuf, a * CHUNK * 8, pool,
                               (a * NHANDS + c * CHUNK) * 2, CHUNK * 2);
    }
    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, s.bind);
    pass.dispatchWorkgroups(groups);
    pass.end();
    enc.copyBufferToBuffer(s.outBuf, 0, s.staging, 0, CHUNK * 4);
    device.queue.submit([enc.finish()]);
    const at = c * CHUNK;
    s.busy = s.staging.mapAsync(GPUMapMode.READ).then(() => {
      dest.set(new Uint32Array(s.staging.getMappedRange()), at);
      s.staging.unmap();
      s.busy = null;
    });
  }
  for (const s of slots) {
    if (s.busy) await s.busy;
  }
}

// One caller-built pool through the pipelined path, values returned in
// hand order. bench_eval_test.js uses this to check that the chunk machinery lands
// every value at its hand's index -- the property the benchmark's own
// checksum, being a sum, cannot see.
export async function evalPool(
  cfg: BenchConfig, pool: Uint32Array<ArrayBuffer>,
): Promise<Uint32Array> {
  const { device } = await getGPU();
  const src = await loadKernel(cfg.kernel);
  const { nmask, lanes } = parseMeta(src);
  const pipeline = await makePipeline(device, src, "main_eval");
  const { slots, destroy } = makeSlots(device, pipeline, nmask);
  const dest = new Uint32Array(NHANDS);
  try {
    await onePass(device, pipeline, CHUNK / (32 * lanes) / WG, nmask, slots, pool, dest);
  } finally {
    destroy();
  }
  return dest;
}

// off the clock -- see the note at the top of the file
function checksum(v: Uint32Array): number {
  let acc = 0;
  for (let j = 0; j < v.length; j++) acc = (acc + v[j]) >>> 0;
  return acc;
}

const yieldToPage = () => new Promise<void>((r) => setTimeout(r, 0));

// Evaluate an explicit list of hands through main_eval: the benchmark's
// inner loop with the pools and the timing stripped out. `hands` is one
// entry per hand, each an array of card-id arrays -- [[c0..c6]] for hold'em,
// [[hole...], [board...]] for omaha -- and the result is one value per hand.
//
// bench_eval_test.js drives this against the from-the-rules reference evaluators.
// The circuit's output is order-isomorphic rather than equal to a reference
// score, so what a check can assert is that every pair of hands orders the
// same way; that agreement is what licenses quoting ns/hand from these
// kernels next to the C harness's.
export async function evalHands(cfg: BenchConfig, hands: number[][][]): Promise<Uint32Array> {
  const { device } = await getGPU();
  const src = await loadKernel(cfg.kernel);
  const { nmask, lanes } = parseMeta(src);

  // an invocation owns a whole block of hands, so the buffer is rounded up;
  // the padding hands are empty masks and their values are discarded
  const per = 32 * lanes;
  const n = Math.ceil(hands.length / per) * per;
  const pool = new Uint32Array(nmask * n * 2);
  hands.forEach((masks, i) => {
    masks.forEach((cards, a) => {
      const o = 2 * (a * n + i);
      for (const id of cards) pool[o + (id >> 5)] |= 1 << (id & 31);
    });
  });

  const inBuf = device.createBuffer({
    size: pool.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  const outBuf = device.createBuffer({
    size: n * 4,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
  });
  const staging = device.createBuffer({
    size: n * 4,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
  try {
    device.queue.writeBuffer(inBuf, 0, pool);
    const pipeline = await makePipeline(device, src, "main_eval");
    const bind = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: inBuf } },
        { binding: 1, resource: { buffer: outBuf } },
      ],
    });
    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bind);
    pass.dispatchWorkgroups(Math.ceil(n / per / WG));
    pass.end();
    enc.copyBufferToBuffer(outBuf, 0, staging, 0, n * 4);
    device.queue.submit([enc.finish()]);
    await staging.mapAsync(GPUMapMode.READ);
    const vals = new Uint32Array(staging.getMappedRange()).slice(0, hands.length);
    staging.unmap();
    return vals;
  } finally {
    inBuf.destroy();
    outBuf.destroy();
    staging.destroy();
  }
}

// onProgress(configKey, order, "pool" | "warm" | "run" | row)
export async function runBenchmarks(
  onProgress?: (key: string, order: string, what: BenchPhase | BenchRow) => void,
  cancelled: () => boolean = () => false,
): Promise<BenchResults> {
  const { adapter, device } = await getGPU();
  const results: BenchResults = { adapter: adapterLabel(adapter), rows: [] };
  const pipes = new Map<string, GPUComputePipeline>();
  // the caller's landing buffer, allocated once -- the C harness's static
  // `vals[BS_BATCH]`, scaled to a whole pool
  const dest = new Uint32Array(NHANDS);

  for (const cfg of CONFIGS) {
    if (cancelled()) break;
    const src = await loadKernel(cfg.kernel);
    const { nmask, lanes } = parseMeta(src);
    // hold'em packs all seven cards into one mask, omaha splits hole and
    // board; if that ever stops matching the circuit, the pools are wrong
    if (nmask !== (cfg.k ? 2 : 1)) {
      throw new Error(`${cfg.kernel}: nmask=${nmask} does not fit ${cfg.key}`);
    }

    // compilation and pipeline creation stay off the clock
    if (!pipes.has(cfg.kernel)) {
      pipes.set(cfg.kernel, await makePipeline(device, src, "main_eval"));
    }
    const pipeline = pipes.get(cfg.kernel)!;
    const groups = CHUNK / (32 * lanes) / WG;
    const { slots, destroy } = makeSlots(device, pipeline, nmask);

    try {
      for (const order of ORDERS) {
        if (cancelled()) break;
        onProgress?.(cfg.key, order, "pool");
        await yieldToPage();
        const pool = buildPool(cfg, order);

        onProgress?.(cfg.key, order, "warm");
        // two warmups: first-touch buffer costs, and a pipeline's first
        // dispatch can still be compiling
        await onePass(device, pipeline, groups, nmask, slots, pool, dest);
        await onePass(device, pipeline, groups, nmask, slots, pool, dest);
        const want = checksum(dest); // off the clock

        const c0 = performance.now();
        await onePass(device, pipeline, groups, nmask, slots, pool, dest);
        await onePass(device, pipeline, groups, nmask, slots, pool, dest);
        const perPass = (performance.now() - c0) / 2;
        const reps = Math.max(MIN_REPS,
          Math.min(MAX_REPS, Math.round(TARGET_MS / perPass)));

        onProgress?.(cfg.key, order, "run");
        const t0 = performance.now();
        for (let r = 0; r < reps; r++) {
          await onePass(device, pipeline, groups, nmask, slots, pool, dest);
        }
        const ms = performance.now() - t0;

        // the pool is fixed, so the last pass's values must match the
        // first's; drift means the pipeline is not what we think
        const got = checksum(dest);
        if (got !== want) {
          throw new Error(`${cfg.key}/${order}: checksum drift ${want} -> ${got}`);
        }

        const hands = reps * NHANDS;
        const row: BenchRow = {
          config: cfg.key, order, reps, checksum: want,
          nsPerHand: (ms * 1e6) / hands,
          handsPerSec: hands / (ms / 1000),
        };
        results.rows.push(row);
        onProgress?.(cfg.key, order, row);
        await yieldToPage();
      }
    } finally {
      destroy();
    }
  }
  return results;
}
