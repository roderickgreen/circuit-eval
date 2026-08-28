// End-to-end check of the value kernels' front door (main_eval): card masks
// in, one value per hand out, with the transposes and the card -> plane map
// run in-shader. Needs a WebGPU adapter:
//
//   cd verify/webgpu && ~/.deno/bin/deno run --allow-read \
//     --unstable-webgpu validate/bench_eval_test.js
//
// The circuit's 24-bit output is order-isomorphic to a hand's strength, not
// equal to any particular score, so the assertion is that every *pair* of
// hands orders the same way the from-the-rules reference orders them --
// including ties, which is the half that catches a truncated encoding.
//
// This is what licenses quoting bench.ts's ns/hand next to the C harness's
// numbers: without it the benchmark only proves the GPU can move bytes
// quickly.

import { evalHands, evalPool, buildPool, CONFIGS, POOL_HANDS, CHUNK } from "./bench.ts";
import { scoreHoldem, scoreOmahaHigh } from "./ref_eval.js";

// deterministic sample, so a failure is reproducible
function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
}

function deal(next, counts) {
  const deck = Int32Array.from({ length: 52 }, (_, i) => i);
  const out = [];
  let t = 0;
  for (const n of counts) {
    const group = [];
    for (let j = 0; j < n; j++, t++) {
      const k = t + (next() % (52 - t));
      const tmp = deck[t]; deck[t] = deck[k]; deck[k] = tmp;
      group.push(deck[t]);
    }
    out.push(group);
  }
  return out;
}

// A few hands chosen to sit on category boundaries, where an off-by-one in
// the encoding shows up: card id = 4*rank + suit, rank 0 = deuce.
const card = (r, s) => 4 * r + s;
const HOLDEM_FIXTURES = [
  [[card(12, 0), card(0, 0), card(1, 0), card(2, 0), card(3, 0), card(7, 1), card(9, 2)]], // five-high straight flush
  [[card(12, 0), card(0, 1), card(1, 2), card(2, 3), card(3, 0), card(7, 1), card(9, 2)]], // wheel
  [[card(12, 0), card(11, 0), card(10, 0), card(9, 0), card(8, 0), card(2, 1), card(3, 2)]], // royal
  [[card(5, 0), card(5, 1), card(5, 2), card(5, 3), card(9, 0), card(2, 1), card(3, 2)]], // quads
  [[card(5, 0), card(5, 1), card(5, 2), card(9, 3), card(9, 0), card(2, 1), card(3, 2)]], // boat
  [[card(12, 0), card(10, 0), card(8, 0), card(6, 0), card(4, 0), card(2, 1), card(3, 2)]], // flush
  [[card(0, 0), card(1, 1), card(2, 2), card(3, 3), card(5, 0), card(7, 1), card(9, 2)]], // high card
];

const SAMPLES = { holdem: 400, omaha4: 200, omaha5: 200, omaha6: 200 };

function checkOrdering(vals, refs) {
  // every pair must agree in sign, ties included
  const bad = [];
  for (let i = 0; i < vals.length && bad.length < 5; i++) {
    for (let j = i + 1; j < vals.length; j++) {
      const g = Math.sign(vals[i] - vals[j]);
      const r = Math.sign(refs[i] - refs[j]);
      if (g !== r) { bad.push({ i, j, gpu: [vals[i], vals[j]], ref: [refs[i], refs[j]] }); break; }
    }
  }
  return bad;
}

// Read hand i's card lists back out of a benchmark pool.
function poolHand(pool, nmask, i) {
  const masks = [];
  for (let a = 0; a < nmask; a++) {
    const cards = [];
    const o = 2 * (a * POOL_HANDS + i);
    for (let w = 0; w < 2; w++) {
      for (let m = pool[o + w]; m; m &= m - 1) {
        cards.push(32 * w + (31 - Math.clz32(m & -m)));
      }
    }
    masks.push(cards);
  }
  return masks;
}

// The benchmark's inner loop (evalPool / onePass) walks the pool in
// chunks through a ring of buffer slots, and its checksum is a sum -- a
// chunk landed at the wrong offset would pass it. So the placement is
// checked here instead: hands straddling every chunk boundary, plus the
// ends, evaluated a second time through the one-dispatch path and
// compared value-for-value (same circuit, so the values must be equal,
// not merely order-alike).
async function runPoolPathTest(cfg, log) {
  const nmask = cfg.k ? 2 : 1;
  const pool = buildPool(cfg, "random");
  const vals = await evalPool(cfg, pool);

  const idx = [0, POOL_HANDS - 1];
  for (let b = CHUNK; b < POOL_HANDS; b += CHUNK) idx.push(b - 1, b);
  const hands = idx.map((i) => poolHand(pool, nmask, i));
  const single = await evalHands(cfg, hands);

  let bad = 0;
  idx.forEach((i, j) => {
    if (vals[i] !== single[j]) bad++;
  });
  const ok = bad === 0;
  log(`${ok ? "ok  " : "FAIL"} ${cfg.key} pool path: ${idx.length} boundary hands, `
    + `${bad} misplaced values`);
  return ok;
}

export async function runEvalTest({ log = console.log } = {}) {
  const report = [];
  for (const cfg of CONFIGS) {
    const next = rng(0x9e3779b9);
    const counts = cfg.k ? [cfg.k, 5] : [7];
    const hands = cfg.k ? [] : HOLDEM_FIXTURES.slice();
    while (hands.length < SAMPLES[cfg.key]) hands.push(deal(next, counts));

    const vals = await evalHands(cfg, hands);
    const refs = hands.map((m) =>
      cfg.k ? scoreOmahaHigh(m[0], m[1]) : scoreHoldem(m[0], []));

    const bad = checkOrdering(vals, refs);
    const distinct = new Set(vals).size;
    const row = {
      config: cfg.key,
      hands: hands.length,
      distinctValues: distinct,
      mismatches: bad.length,
      ok: bad.length === 0 && distinct > 1,
      examples: bad.slice(0, 3),
    };
    report.push(row);
    log(`${row.ok ? "ok  " : "FAIL"} ${cfg.key}: ${row.hands} hands, `
      + `${distinct} distinct values, ${bad.length} ordering mismatches`);
    for (const b of row.examples) log(`     hands ${b.i}/${b.j} gpu ${b.gpu} ref ${b.ref}`);
  }
  // one nmask=1 and one nmask=2 config exercise both pool layouts
  let poolOk = true;
  for (const cfg of [CONFIGS[0], CONFIGS[1]]) {
    poolOk = (await runPoolPathTest(cfg, log)) && poolOk;
  }

  const ok = report.every((r) => r.ok) && poolOk;
  log(ok ? "\nmain_eval agrees with the reference on every pair"
         : "\nmain_eval DISAGREES with the reference");
  return { ok, report };
}

// headless entry; the browser path (importing this module from a page and
// calling runEvalTest yourself) still works, it just isn't the default
if (import.meta.main) {
  await import("./kernel_fetch.js");
  const { ok } = await runEvalTest();
  Deno.exit(ok ? 0 : 1);
}
