// Cross-compare (xc) vs fused-kernel throughput on both-unknown spots.
// Each row runs one spot through one pinned formulation under the hooked
// path (the one the browser demo uses) with a wall-clock budget; big
// spaces report the truncated-run rate, small ones the full-run rate.
// The last argument may override the per-row budget in ms (default 10000).
//
//   cd verify/webgpu && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
//     ~/.deno/bin/deno run --allow-read --unstable-webgpu validate/perf_probe_xc.js [ms]
//
// On a Mac, drop the VK_ICD pin (deno's wgpu uses Metal directly).

await import("./kernel_fetch.js");
const { computeEquity } = await import("../../demo/src/lib/equity.ts");

const card = (s) => "23456789TJQKA".indexOf(s[0]) * 4 + "shdc".indexOf(s[1]);
const cards = (str) => (str ? str.split(" ").map(card) : []);
const BUDGET = +Deno.args[0] || 10000;

async function measure(label, spot) {
  const t0 = performance.now();
  let last = null;
  let full = null;
  try {
    full = await computeEquity({ ...spot,
      onProgress: (p) => { last = p; },
      cancelled: () => performance.now() - t0 > BUDGET });
  } catch (e) {
    if (e.message !== "cancelled") throw e;
    // let queued snapshots drain out of the measurement
  }
  const ms = full ? full.ms : performance.now() - t0;
  const deals = full ? full.boards : (last ? last.done : 0);
  console.log(`${label}: ${(deals / ms / 1e3).toFixed(2)} M deals/s` +
    ` (${full ? "full" : "truncated"}, ${deals.toLocaleString()} deals, ${ms.toFixed(0)} ms)`);
}

// fused pins reproduce the pre-xc router: board-major on preflop-shaped
// spaces, deal-major postflop
const SPOTS = [
  // the shape that started this: one card unknown on each side
  ["holdem preflop 1+1 (4.4G)", {
    variant: "holdem", hero: cards("As"), heroDraw: 1,
    villain: cards("Kd"), villainDraw: 1, board: [],
  }, "boardMajor"],
  ["holdem flop 1+1 (2.1M)", {
    variant: "holdem", hero: cards("As"), heroDraw: 1,
    villain: cards("Kd"), villainDraw: 1, board: cards("Qs 8d 3c"),
  }, "dealMajor"],
  // the base both-unknown case: random vs random
  ["holdem preflop 2+2 rvr (2.8T)", {
    variant: "holdem", hero: [], heroDraw: 2, villain: [], villainDraw: 2,
    board: [],
  }, "boardMajor"],
  ["holdem flop 2+2 rvr (1.3G)", {
    variant: "holdem", hero: [], heroDraw: 2, villain: [], villainDraw: 2,
    board: cards("Qs 8d 3c"),
  }, "dealMajor"],
  // context: the already-fast single-side-unknown baseline (not routed xc)
  ["holdem preflop 0+2 AsAh vs random (2.1G)", {
    variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
    board: [],
  }, null],
  // omaha hi-lo: half-known 2+2 and the fully-random postflop shape
  ["omaha hilo flop 2+2 half-known (733M)", {
    variant: "omaha", himode: "hilo", hero: cards("As 2d"), heroDraw: 2,
    villain: cards("Ah 3c"), villainDraw: 2, board: cards("Qs 8d 4c"),
  }, "dealMajor"],
  ["omaha hilo flop 4+4 rvr (2.7e13)", {
    variant: "omaha", himode: "hilo", hero: [], heroDraw: 4,
    villain: [], villainDraw: 4, board: cards("Qs 8d 3c"),
  }, "boardMajor"],
];

for (const [label, spot, fusedPin] of SPOTS) {
  if (fusedPin) {
    await measure(`${label} fused/${fusedPin === "boardMajor" ? "bm" : "dm"}`,
      { ...spot, [fusedPin]: true });
    await measure(`${label} xc      `, { ...spot, xCompare: true });
  } else {
    await measure(`${label} routed`, spot);
  }
}
