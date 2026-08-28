// Board-major (R1) wiring probe: computeEquity routes villain-unknown spots
// by board count — deal-major below 32K boards (postflop), board-major above
// (preflop-shaped spaces) and for v > 5. Cross-check exact tallies between
// the two formulations, then time the flagship spots through the routed
// path — exactly what the browser demo runs.
//
//   cd verify/webgpu && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
//     ~/.deno/bin/deno run --allow-read --unstable-webgpu validate/perf_probe_bm.js

await import("./kernel_fetch.js");
const { computeEquity } = await import("../../demo/src/lib/equity.ts");

const card = (s) => "23456789TJQKA".indexOf(s[0]) * 4 + "shdc".indexOf(s[1]);
const cards = (str) => (str ? str.split(" ").map(card) : []);

async function crossCheck(label, spot) {
  const dm = await computeEquity({ ...spot, dealMajor: true });
  const bm = await computeEquity({ ...spot, boardMajor: true });
  const ok = JSON.stringify(dm.raw) === JSON.stringify(bm.raw);
  console.log(`${label}: ${ok ? "MATCH" : "MISMATCH"}` +
    ` dm=${JSON.stringify(dm.raw)} bm=${JSON.stringify(bm.raw)}` +
    ` dm ${dm.ms.toFixed(0)} ms, bm ${bm.ms.toFixed(0)} ms`);
  if (!ok) throw new Error(`${label}: tallies diverge`);
}

async function time(label, spot) {
  const r = await computeEquity(spot);
  console.log(`${label}: ${r.boards.toLocaleString()} deals in ${r.ms.toFixed(0)} ms` +
    ` = ${(r.boards / r.ms / 1e6).toFixed(2)} G deals/s (hero ${(100 * r.hero).toFixed(2)}%)`);
}

// --- holdem: one spot per street shape (k=2, k=1), then the flagship k=5
await crossCheck("holdem flop  As Ah / Ks 7d 2c", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
  board: cards("Ks 7d 2c"),
});
await crossCheck("holdem turn  Kd 7d / Qs Jh 4d 4c", {
  variant: "holdem", hero: cards("Kd 7d"), villain: [], villainDraw: 2,
  board: cards("Qs Jh 4d 4c"),
});
await crossCheck("holdem preflop AsAh vs random (2.1G)", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2, board: [],
});

// --- omaha: postflop spaces (dm territory) and the preflop-shaped space
// where board-major pays (1.37M boards; hilo would overflow the quarter
// tallies at this size, so the big cross-check is high-only)
await crossCheck("omaha4 flop 9876 vs random (122M)", {
  variant: "omaha", himode: "high", hero: cards("9s 8d 7h 6c"), villain: [],
  villainDraw: 4, board: cards("Ts Jd Ac"),
});
await crossCheck("omaha4 hilo flop A2KQ vs random (122M)", {
  variant: "omaha", himode: "hilo", hero: cards("As 2d Kh Qc"), villain: [],
  villainDraw: 4, board: cards("6s 7d Jc"),
});
await crossCheck("omaha4 preflop AAxx villain half-known (1.1G)", {
  variant: "omaha", himode: "high", hero: cards("9s 8d 7h 6c"),
  villain: cards("As Ad"), villainDraw: 2, board: [],
});

// --- timings via the routed path (warmed by the cross-checks above)
await time("holdem preflop AsAh vs random", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2, board: [],
});
await time("omaha4 flop 9876 vs random", {
  variant: "omaha", himode: "high", hero: cards("9s 8d 7h 6c"), villain: [],
  villainDraw: 4, board: cards("Ts Jd Ac"),
});
await time("omaha4 preflop 9876 vs AAxx half-known", {
  variant: "omaha", himode: "high", hero: cards("9s 8d 7h 6c"),
  villain: cards("As Ad"), villainDraw: 2, board: [],
});
