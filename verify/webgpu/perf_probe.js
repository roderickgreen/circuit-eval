// Native perf probe: bench-kernel rate vs equity-kernel rate on the
// same adapter, so equity slowdowns can be diagnosed locally instead of
// round-tripping through a browser on other hardware.
//
//   cd verify/webgpu && sg render -c "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
//     ~/.deno/bin/deno run --allow-read --unstable-webgpu validate/perf_probe.js [--bench]"
//
// Default: equity spots only (fast). --bench adds the front-door value-kernel
// benchmark (masks in, values out -- the ns/hand the C harness quotes).

await import("./kernel_fetch.js");
const { computeEquity } = await import("../../demo/src/lib/equity.ts");

const card = (s) => "23456789TJQKA".indexOf(s[0]) * 4 + "shdc".indexOf(s[1]);
const cards = (str) => str.split(" ").map(card);

if (Deno.args.includes("--bench")) {
  const { runBenchmarks } = await import("./bench.ts");
  const r = await runBenchmarks();
  console.log(`adapter: ${r.adapter || "(unnamed)"}`);
  for (const row of r.rows) {
    console.log(`bench ${row.config}/${row.order}: ${row.nsPerHand.toFixed(3)} ns/hand`
      + ` (${(row.handsPerSec / 1e9).toFixed(2)} G hands/s, ${row.reps} passes)`);
  }
}

async function time(label, spot) {
  await computeEquity(spot); // warm: pipeline compile + buffers
  const r = await computeEquity(spot);
  const evals = 2 * r.boards;
  console.log(`${label}: ${r.boards.toLocaleString()} deals in ${r.ms.toFixed(0)} ms` +
    ` = ${(evals / r.ms / 1e6).toFixed(2)} G evals/s`);
  return r;
}

// v=0 baseline: the phase-2 shape (1.7M boards)
await time("holdem preflop known (v=0)", {
  variant: "holdem", hero: cards("As Ah"), villain: cards("Ks Kh"), board: [],
});
// the reported-slow spot: 2.1G deals, v=2
await time("holdem preflop vs random (v=2)", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2, board: [],
});
// omaha for cross-variant signal (v=0 preflop)
await time("omaha4 preflop known (v=0)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad Kh Qc"), villain: cards("Ks Kd 8h 7c"), board: [],
});
