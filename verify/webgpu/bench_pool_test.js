// The GPU benchmark's pools have to be the same hands the C harness runs,
// or the ns/hand figures are not comparable to the published CPU numbers.
// Everything here re-derives the C generators from their definitions --
// BigInt for the 64-bit LCG, a plain combination walk for colex order --
// rather than restating bench.ts's arithmetic in a second form.
//
//   deno run --allow-read verify/webgpu/bench_pool_test.js
//   node verify/webgpu/bench_pool_test.js

import { buildPool, POOL_HANDS, LCG, gosper } from "./bench.ts";

let failures = 0;
function check(name, ok, detail = "") {
  if (!ok) failures++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? "  " + detail : ""}`);
}

// ---- the LCG -------------------------------------------------------------
// bench/holdem.cc: state = state * 6364136223846793005 + 1442695040888963407,
// draw = state >> 33, seeded 0x9e3779b97f4a7c15.

const M64 = (1n << 64n) - 1n;
function* refLCG() {
  let s = 0x9e3779b97f4a7c15n;
  for (;;) {
    s = (s * 6364136223846793005n + 1442695040888963407n) & M64;
    yield Number(s >> 33n);
  }
}

{
  const ref = refLCG(), got = new LCG();
  let bad = -1;
  for (let i = 0; i < 100000; i++) {
    if (got.next() !== ref.next().value) { bad = i; break; }
  }
  check("LCG matches the C generator over 100k draws", bad < 0,
        bad < 0 ? "" : `first divergence at draw ${bad}`);
}

// ---- Gosper's hack -------------------------------------------------------
// Next combination in colex order, checked against an independent walk that
// enumerates k-subsets of 0..51 directly.

function* refCombos(k) {
  const c = Array.from({ length: k }, (_, i) => i);
  for (;;) {
    yield c.reduce((m, i) => m | (1 << i), 0) >>> 0;
    let i = 0;
    while (i < k && (i + 1 === k ? 51 : c[i + 1] - 1) <= c[i]) i++;
    if (i === k) return;
    c[i]++;
    for (let j = 0; j < i; j++) c[j] = j;
  }
}

for (const k of [7, 9, 11]) {
  const ref = refCombos(k);
  let x = ((1 << k) - 1) >>> 0, bad = -1;
  for (let i = 0; i < POOL_HANDS; i++) {
    if (x !== ref.next().value) { bad = i; break; }
    x = gosper(x);
  }
  check(`gosper walks colex order for k=${k} over 2^20 combinations`, bad < 0,
        bad < 0 ? `top bit ${31 - Math.clz32(x & -x ? x : 1)}` : `diverged at ${bad}`);
}

// ---- pool shape ----------------------------------------------------------

const bits = (lo, hi) => {
  const ids = [];
  for (let b = 0; b < 32; b++) if (lo & (1 << b)) ids.push(b);
  for (let b = 0; b < 20; b++) if (hi & (1 << b)) ids.push(32 + b);
  return ids;
};

// mask a of hand i lives at vec2 index a*POOL_HANDS + i
const maskOf = (pool, a, i) =>
  bits(pool[2 * (a * POOL_HANDS + i)], pool[2 * (a * POOL_HANDS + i) + 1]);

const CASES = [
  { cfg: { key: "holdem", k: 0 }, nmask: 1, counts: [7] },
  { cfg: { key: "omaha4", k: 4 }, nmask: 2, counts: [4, 5] },
  { cfg: { key: "omaha5", k: 5 }, nmask: 2, counts: [5, 5] },
  { cfg: { key: "omaha6", k: 6 }, nmask: 2, counts: [6, 5] },
];

for (const { cfg, nmask, counts } of CASES) {
  for (const order of ["sequential", "random"]) {
    const pool = buildPool(cfg, order);
    const tag = `${cfg.key}/${order}`;

    check(`${tag}: buffer is ${nmask} mask array(s) of 2^20 vec2`,
          pool.length === nmask * POOL_HANDS * 2, `${pool.length} words`);

    // every hand: right card count per mask, no card in two masks, all ids
    // inside the 52-card deck
    let badCount = -1, badOverlap = -1, badRange = -1;
    for (let i = 0; i < POOL_HANDS; i++) {
      const ms = counts.map((_, a) => maskOf(pool, a, i));
      for (let a = 0; a < nmask; a++) {
        if (ms[a].length !== counts[a] && badCount < 0) badCount = i;
        if (ms[a].some((c) => c > 51) && badRange < 0) badRange = i;
      }
      if (nmask === 2 && badOverlap < 0
          && ms[0].some((c) => ms[1].includes(c))) badOverlap = i;
      if (badCount >= 0 && badOverlap >= 0 && badRange >= 0) break;
    }
    check(`${tag}: every hand has ${counts.join(" + ")} distinct cards`,
          badCount < 0, badCount < 0 ? "" : `hand ${badCount}`);
    check(`${tag}: card ids stay in 0..51`, badRange < 0,
          badRange < 0 ? "" : `hand ${badRange}`);
    if (nmask === 2) {
      check(`${tag}: hole and board are disjoint`, badOverlap < 0,
            badOverlap < 0 ? "" : `hand ${badOverlap}`);
    }

    if (order === "sequential") {
      // bench/omaha.cc's split: hole = the combination's top k, board = its
      // bottom 5. Hold'em's single mask is just the combination.
      const all = counts.map((_, a) => maskOf(pool, a, 0)).flat().sort((x, y) => x - y);
      const want = Array.from({ length: counts.reduce((a, b) => a + b) }, (_, j) => j);
      check(`${tag}: first hand is the lowest combination`,
            all.join(",") === want.join(","), all.join(","));
      if (nmask === 2) {
        const hole = maskOf(pool, 0, 0), board = maskOf(pool, 1, 0);
        check(`${tag}: board is the 5 lowest cards, hole the top ${cfg.k}`,
              Math.max(...board) < Math.min(...hole),
              `board ${board} hole ${hole}`);
      }
    } else {
      // a uniform draw should touch the whole deck within the first 10k hands
      const seen = new Set();
      for (let i = 0; i < 10000; i++) {
        for (let a = 0; a < nmask; a++) for (const c of maskOf(pool, a, i)) seen.add(c);
      }
      check(`${tag}: random pool covers all 52 cards`, seen.size === 52,
            `${seen.size} distinct`);
    }
  }
}

console.log(failures ? `\n${failures} check(s) failed` : "\nall checks passed");
if (failures) { throw new Error(`${failures} check(s) failed`); }
