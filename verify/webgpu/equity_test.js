// GPU equity kernels vs the naive JS reference, exact tally equality.
//
//   cd verify/webgpu && ~/.deno/bin/deno run --allow-read \
//     --unstable-webgpu validate/equity_test.js [--full]
//
// Reads the committed demo kernels by default; `make validate-kernels` runs
// it with KERNELS=build/kernels to vet a fresh generation before install.
//
// Default: fixed spots + seeded random spots at flop/turn/river for every
// variant (sub-second spaces). --full adds full-preflop spots (holdem JS
// reference takes ~half a minute; GPU side is milliseconds).

await import("./kernel_fetch.js");
const { computeEquity } = await import("../../demo/src/lib/equity.ts");
const { refEquity, combos } = await import("./ref_eval.js");

const FULL = Deno.args.includes("--full");

// deterministic PRNG for reproducible random spots
let seed = 0xc0ffee;
function rnd() {
  seed ^= seed << 13; seed >>>= 0;
  seed ^= seed >>> 17;
  seed ^= seed << 5; seed >>>= 0;
  return seed / 0x100000000;
}
function randomSpot(nHole, nBoard) {
  const deck = Array.from({ length: 52 }, (_, i) => i);
  for (let i = 51; i > 0; i--) {
    const j = Math.floor(rnd() * (i + 1));
    [deck[i], deck[j]] = [deck[j], deck[i]];
  }
  return {
    hero: deck.slice(0, nHole),
    villain: deck.slice(nHole, 2 * nHole),
    board: deck.slice(2 * nHole, 2 * nHole + nBoard),
  };
}

// "As" -> id; suit order s,h,d,c matches demo/src/lib/cards.ts
const card = (s) => "23456789TJQKA".indexOf(s[0]) * 4 + "shdc".indexOf(s[1]);
const cards = (str) => str.split(" ").map(card);

let nrun = 0, nfail = 0;
async function check(label, spot) {
  const gpu = await computeEquity(spot);
  const ref = refEquity(spot);
  const ok = JSON.stringify(gpu.raw) === JSON.stringify(ref);
  nrun++;
  if (!ok) {
    nfail++;
    console.error(`FAIL ${label}: gpu=${JSON.stringify(gpu.raw)} ref=${JSON.stringify(ref)}`);
  } else {
    console.log(`ok ${label}: hero ${(gpu.hero * 100).toFixed(2)}% over ${gpu.boards} boards (${gpu.ms.toFixed(0)} ms gpu)`);
  }
}

// --- fixed spots with hand-checkable structure ---
await check("holdem river set-over-set", {
  variant: "holdem", hero: cards("As Ah"), villain: cards("Ks Kh"),
  board: cards("Ad Kd 2s 7c 9h"),
});
await check("holdem turn flush draw", {
  variant: "holdem", hero: cards("Ah Kh"), villain: cards("Qs Qd"),
  board: cards("2h 9h Js 3c"),
});
await check("holdem flop AA vs KK", {
  variant: "holdem", hero: cards("As Ah"), villain: cards("Ks Kh"),
  board: cards("2d 7c 9s"),
});
await check("omaha flop wrap vs top set", {
  variant: "omaha", himode: "high",
  hero: cards("9s 8d 7h 6c"), villain: cards("As Ad Kh 2c"),
  board: cards("Ts Jd Ac"),
});
await check("omaha hilo flop A2 vs A3", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Kh Qc"), villain: cards("Ah 3d 4h 5c"),
  board: cards("6s 7d Jc"),
});
await check("omaha hilo river quartered", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th 9c"), villain: cards("Ah 2c Kd Qs"),
  board: cards("3s 4d 8c Kh Jd"),
});

// --- seeded random spots, all variants and streets ---
for (const nBoard of [5, 4, 3]) {
  for (let i = 0; i < 4; i++) {
    await check(`holdem random board${nBoard} #${i}`,
      { variant: "holdem", ...randomSpot(2, nBoard) });
  }
}
for (const nHole of [4, 5, 6, 2, 8]) {
  await check(`omaha${nHole} random flop`,
    { variant: "omaha", himode: "high", ...randomSpot(nHole, 3) });
  await check(`omaha${nHole} hilo random flop`,
    { variant: "omaha", himode: "hilo", ...randomSpot(nHole, 3) });
}
for (let i = 0; i < 3; i++) {
  await check(`omaha4 random turn #${i}`,
    { variant: "omaha", himode: "high", ...randomSpot(4, 4) });
  await check(`omaha4 hilo random river #${i}`,
    { variant: "omaha", himode: "hilo", ...randomSpot(4, 5) });
}

// --- villain partially / fully unknown: naive-ref spots ---
await check("holdem river vs random", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"),
});
await check("holdem river vs random (bm)", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"), boardMajor: true,
});
await check("holdem river villain half-known", {
  variant: "holdem", hero: cards("Jh Th"), villain: cards("Qs"), villainDraw: 1,
  board: cards("8h 9c 2d Qh 2c"),
});
await check("holdem turn vs random", {
  variant: "holdem", hero: cards("Kd Kc"), villain: [], villainDraw: 2,
  board: cards("Ah 7s 2c 5d"),
});
await check("holdem flop villain half-known", {
  variant: "holdem", hero: cards("9s 8s"), villain: cards("Ad"), villainDraw: 1,
  board: cards("7s 6h 2s"),
});
// omaha exercises the drawn-card hole planes (52-plane offset from board).
// Routing picks a kernel by board count, so bm/deal-major are pinned
// explicitly here to keep both formulations naive-validated
await check("omaha river villain 2 drawn (bm)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h 6c"), villain: cards("Ks Kd"), villainDraw: 2,
  board: cards("Kc 8d 4s 2h Jd"), boardMajor: true,
});
await check("omaha river villain 2 drawn (deal-major)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h 6c"), villain: cards("Ks Kd"), villainDraw: 2,
  board: cards("Kc 8d 4s 2h Jd"), dealMajor: true,
});
await check("omaha hilo river villain 2 drawn (bm)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th 9c"), villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh Jd"), boardMajor: true,
});
await check("omaha hilo river villain 2 drawn (deal-major)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th 9c"), villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh Jd"), dealMajor: true,
});
// bm with board cards still to come (k>0: skipb over drawn board slots)
await check("omaha turn villain 2 drawn (bm)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h 6c"), villain: cards("Ks Kd"), villainDraw: 2,
  board: cards("Kc 8d 4s 2h"), boardMajor: true,
});
await check("omaha hilo river villain 3 drawn (bm)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th 9c"), villain: cards("Ah"), villainDraw: 3,
  board: cards("4s 5d 8c Kh Jd"), boardMajor: true,
});

// --- both sides part-unknown: hero draws loop on the uniform axis
// (equity.js), list entries claiming a drawn hero card drop in-kernel.
// Naive-ref spots on small spaces, every kernel formulation pinned ---
await check("holdem river hero Ax vs random", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"),
});
await check("holdem river hero Ax vs random (bm)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"), boardMajor: true,
});
// hero drawn with the villain fully known: v=0 stays deal-major, and the
// drawn hero card must knock board-list lanes out of `valid`
await check("holdem turn hero 1 drawn vs known villain", {
  variant: "holdem", hero: cards("Jh"), heroDraw: 1, villain: cards("Qs Qd"),
  board: cards("8h 9c 2d Qh"),
});
// omaha both-unknown exercises hdraw against the 52-plane hole/board split
await check("omaha river hero 1 drawn villain 1 drawn (bm)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h"), heroDraw: 1, villain: cards("Ks Kd Kc"), villainDraw: 1,
  board: cards("Kh 8d 4s 2h Jd"), boardMajor: true,
});
await check("omaha river hero 1 drawn villain 1 drawn (deal-major)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h"), heroDraw: 1, villain: cards("Ks Kd Kc"), villainDraw: 1,
  board: cards("Kh 8d 4s 2h Jd"), dealMajor: true,
});
await check("omaha hilo river hero 1 drawn villain 1 drawn (bm)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th"), heroDraw: 1, villain: cards("Ah 3c Kd"), villainDraw: 1,
  board: cards("4s 5d 8c Kh Jd"), boardMajor: true,
});
await check("omaha hilo river hero 1 drawn villain 1 drawn (deal-major)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th"), heroDraw: 1, villain: cards("Ah 3c Kd"), villainDraw: 1,
  board: cards("4s 5d 8c Kh Jd"), dealMajor: true,
});

// --- cross-compare formulation: routing sends both-unknown spots there by
// default (covered above); the pins keep the kernel exercised on naive-ref
// spots even if routing changes, and the 1+1 shape is the one the fused
// kernels handle worst ---
await check("holdem river hero 1 drawn villain 1 drawn (xc)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: cards("Ks"),
  villainDraw: 1, board: cards("Kd 7c 2s 9h 3d"), xCompare: true,
});
await check("holdem river hero Ax vs random (xc)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"), xCompare: true,
});
await check("omaha river hero 1 drawn villain 1 drawn (xc)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ad 7h"), heroDraw: 1, villain: cards("Ks Kd Kc"), villainDraw: 1,
  board: cards("Kh 8d 4s 2h Jd"), xCompare: true,
});
await check("omaha hilo river hero 1 drawn villain 1 drawn (xc)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th"), heroDraw: 1, villain: cards("Ah 3c Kd"), villainDraw: 1,
  board: cards("4s 5d 8c Kh Jd"), xCompare: true,
});
// xc vs board-major on a both-fully-unknown space past the naive ref: the
// hero tile axis (1081 heroes = 3 tiles) and the villain knockout planes
// both carry real load here
async function checkXC(label, spot) {
  const x = await computeEquity({ ...spot, xCompare: true });
  const bm = await computeEquity({ ...spot, boardMajor: true });
  nrun++;
  if (JSON.stringify(x.raw) === JSON.stringify(bm.raw)) {
    console.log(`ok ${label}: xc==bm over ${x.boards.toLocaleString()} deals` +
      ` (xc ${x.ms.toFixed(0)} / bm ${bm.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: xc=${JSON.stringify(x.raw)} bm=${JSON.stringify(bm.raw)}`);
  }
}
await checkXC("holdem river both 2 drawn (xc vs bm)", {
  variant: "holdem", hero: [], heroDraw: 2, villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"),
});
await checkXC("omaha hilo river hero 2 drawn villain 2 drawn (xc vs bm)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d"), heroDraw: 2, villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh Jd"),
});

// hero-axis peel: summing (hero+[c], h-1) over every live c counts each
// hero draw exactly h times. Exercises the 4-card hero draws of the
// omaha random-vs-random shape through xc, grounded in the h<=2 spots
// the naive ref and xc==bm identities validate directly
async function checkHeroPeel(label, spot) {
  const whole = await computeEquity(spot);
  const used = new Set([...spot.hero, ...spot.villain, ...spot.board]);
  const tot = {};
  for (let c = 0; c < 52; c++) {
    if (used.has(c)) continue;
    const r = await computeEquity({ ...spot, hero: [...spot.hero, c],
      heroDraw: spot.heroDraw - 1 });
    for (const k of Object.keys(r.raw)) tot[k] = (tot[k] ?? 0) + r.raw[k];
  }
  const scaled = Object.fromEntries(
    Object.keys(whole.raw).map((k) => [k, whole.raw[k] * spot.heroDraw]));
  nrun++;
  if (JSON.stringify(scaled) === JSON.stringify(tot)) {
    console.log(`ok ${label}: x${spot.heroDraw} hero-peel identity over` +
      ` ${whole.boards.toLocaleString()} deals (${whole.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: whole*h=${JSON.stringify(scaled)} peel-sum=${JSON.stringify(tot)}`);
  }
}
await checkHeroPeel("omaha hilo river hero 4 drawn villain 1 drawn (xc hero peel, 5.4M)", {
  variant: "omaha", himode: "hilo", hero: [], heroDraw: 4,
  villain: cards("Ah 3c Kd"), villainDraw: 1, board: cards("4s 5d 8c Kh Jd"),
});

// --- villain-unknown vs the sum over explicit villains through the
// already-validated v=0 path: exact tally identity ---
async function checkSum(label, spot) {
  const whole = await computeEquity(spot);
  const used = new Set([...spot.hero, ...spot.villain, ...spot.board]);
  const live = [];
  for (let c = 0; c < 52; c++) if (!used.has(c)) live.push(c);
  const tot = {};
  for (const draw of combos(live, spot.villainDraw)) {
    const r = await computeEquity({
      ...spot, villain: [...spot.villain, ...draw], villainDraw: 0,
    });
    for (const k of Object.keys(r.raw)) tot[k] = (tot[k] ?? 0) + r.raw[k];
  }
  nrun++;
  if (JSON.stringify(whole.raw) === JSON.stringify(tot)) {
    console.log(`ok ${label}: matches per-villain sum over ${whole.boards} deals (${whole.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: whole=${JSON.stringify(whole.raw)} sum=${JSON.stringify(tot)}`);
  }
}
await checkSum("holdem turn vs random (sum identity)", {
  variant: "holdem", hero: cards("Ah Kh"), villain: [], villainDraw: 2,
  board: cards("2h 9h Js 3c"),
});
await checkSum("omaha hilo turn villain 2 drawn (sum identity)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th 9c"), villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh"),
});

// --- both-unknown vs the sum over explicit hero completions through the
// already-validated villain-unknown path: exact tally identity on spaces
// past the naive ref (the flop spots also exercise the board-vs-hdraw
// drop, which only exists at k > 0) ---
async function checkHeroSum(label, spot) {
  const whole = await computeEquity(spot);
  const used = new Set([...spot.hero, ...spot.villain, ...spot.board]);
  const live = [];
  for (let c = 0; c < 52; c++) if (!used.has(c)) live.push(c);
  const tot = {};
  for (const draw of combos(live, spot.heroDraw)) {
    const r = await computeEquity({ ...spot, hero: [...spot.hero, ...draw],
      heroDraw: 0, boardMajor: false, dealMajor: false });
    for (const k of Object.keys(r.raw)) tot[k] = (tot[k] ?? 0) + r.raw[k];
  }
  nrun++;
  if (JSON.stringify(whole.raw) === JSON.stringify(tot)) {
    console.log(`ok ${label}: matches per-hero sum over ${whole.boards.toLocaleString()} deals (${whole.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: whole=${JSON.stringify(whole.raw)} sum=${JSON.stringify(tot)}`);
  }
}
await checkHeroSum("holdem flop hero Ax vs random (hero-sum identity)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("2d 7c 9s"),
});
await checkHeroSum("holdem flop hero Ax vs random (bm, hero-sum identity)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("2d 7c 9s"), boardMajor: true,
});
await checkHeroSum("omaha hilo turn hero 1 drawn villain 2 drawn (hero-sum identity)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th"), heroDraw: 1, villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh"),
});

// --- board-major vs deal-major on spaces too big for the naive ref:
// exact tally identity between two very different GPU formulations
// (deal-major itself is naive-validated on the small spots above) ---
async function checkDM(label, spot) {
  const bm = await computeEquity({ ...spot, boardMajor: true });
  const dm = await computeEquity({ ...spot, dealMajor: true });
  nrun++;
  if (JSON.stringify(bm.raw) === JSON.stringify(dm.raw)) {
    console.log(`ok ${label}: bm==dm over ${bm.boards} deals` +
      ` (bm ${bm.ms.toFixed(0)} / dm ${dm.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: bm=${JSON.stringify(bm.raw)} dm=${JSON.stringify(dm.raw)}`);
  }
}
await checkDM("omaha4 turn vs random (4M)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ks Qd Jc"), villain: [], villainDraw: 4,
  board: cards("Ts 9h 3d 2c"),
});
await checkDM("omaha4 hilo turn vs random (4M)", {
  variant: "omaha", himode: "hilo",
  hero: cards("Ah 2h 3s Kd"), villain: [], villainDraw: 4,
  board: cards("4c 5h 8s Qd"),
});
await checkDM("omaha5 river vs random (v=5)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ks Qs Jd Tc"), villain: [], villainDraw: 5,
  board: cards("9h 8h 2c 3d 7s"),
});
await checkDM("omaha4 hilo flop villain half-known (740K)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2s 3d Kh"), villain: cards("Ad 2d"), villainDraw: 2,
  board: cards("5c 6h Qs"),
});
await checkDM("omaha4 river hero 1 drawn vs random (5.4M)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ks Qd"), heroDraw: 1, villain: [], villainDraw: 4,
  board: cards("Ts 9h 3d 2c 8c"),
});

// --- v=6 peel identity: summing (villain+[c], v-1) over every live c
// counts each villain combo exactly v times, cross-checking a big villain
// list against many small ones. The whole-spot side runs hooked so the
// chunked path is exercised too ---
async function checkPeel(label, spot) {
  const whole = await computeEquity({ ...spot, onProgress: () => {} });
  const used = new Set([...spot.hero, ...spot.villain, ...spot.board]);
  const tot = {};
  for (let c = 0; c < 52; c++) {
    if (used.has(c)) continue;
    const r = await computeEquity({ ...spot, villain: [...spot.villain, c],
      villainDraw: spot.villainDraw - 1, dealMajor: true });
    for (const k of Object.keys(r.raw)) tot[k] = (tot[k] ?? 0) + r.raw[k];
  }
  const scaled = Object.fromEntries(
    Object.keys(whole.raw).map((k) => [k, whole.raw[k] * spot.villainDraw]));
  nrun++;
  if (JSON.stringify(scaled) === JSON.stringify(tot)) {
    console.log(`ok ${label}: x${spot.villainDraw} peel identity over` +
      ` ${whole.boards.toLocaleString()} deals (${whole.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: whole*v=${JSON.stringify(scaled)} peel-sum=${JSON.stringify(tot)}`);
  }
}
await checkPeel("omaha6 river vs random (v=6, 4.5M)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ks Qs Jh Td 9c"), villain: [], villainDraw: 6,
  board: cards("2c 3d 7h 8s Kd"),
});

// --- progress/cancellation hooks: the hooked path chunks the grid and
// snapshots the tally per chunk — same exact tallies as the single-readback
// path, and the deals-done counter must land on the total ---
async function checkHooked(label, spot) {
  const plain = await computeEquity(spot);
  let ticks = 0, last = null;
  const hooked = await computeEquity({ ...spot,
    onProgress: (p) => { ticks++; last = p; } });
  nrun++;
  if (JSON.stringify(plain.raw) === JSON.stringify(hooked.raw) &&
      ticks >= 1 && last.done === last.total && last.total === hooked.boards) {
    console.log(`ok ${label}: ${ticks} progress ticks, tallies match (${hooked.ms.toFixed(0)} ms gpu)`);
  } else {
    nfail++;
    console.error(`FAIL ${label}: plain=${JSON.stringify(plain.raw)} ` +
      `hooked=${JSON.stringify(hooked.raw)} ticks=${ticks} last=${JSON.stringify(last)}`);
  }
}
// multi-chunk spot (2.1G deals -> several cadence-sized chunks)
await checkHooked("holdem preflop vs random hooked (2.1G)", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
  board: [],
});
// hi-lo reads the deals counter from a different slot
await checkHooked("omaha4 hilo turn vs random hooked", {
  variant: "omaha", himode: "hilo",
  hero: cards("Ah 2h 3s Kd"), villain: [], villainDraw: 4,
  board: cards("4c 5h 8s Qd"),
});
// hooked + board-major = villain-axis slices: every slice walks the
// villain list from a nonzero gbase, so exact equality with the plain
// (single-dispatch, gbase=0 full-span) bm run proves the slice bounds and
// accumulation. River spots keep the board side trivial (1 board), so the
// tallies decompose purely along the villain axis being sliced.
await checkHooked("holdem river vs random hooked (bm slices)", {
  variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"), boardMajor: true,
});
await checkHooked("omaha4 turn vs random hooked (bm slices)", {
  variant: "omaha", himode: "high",
  hero: cards("As Ks Qd Jc"), villain: [], villainDraw: 4,
  board: cards("Ts 9h 3d 2c"), boardMajor: true,
});
await checkHooked("omaha4 hilo turn vs random hooked (bm slices)", {
  variant: "omaha", himode: "hilo",
  hero: cards("Ah 2h 3s Kd"), villain: [], villainDraw: 4,
  board: cards("4c 5h 8s Qd"), boardMajor: true,
});
// both-unknown hooked: the chunk schedule interleaves hero draws inside
// every villain-axis span (bm) / grid stride (deal-major); exact equality
// with the plain run proves the interleave covers each draw exactly once
await checkHooked("holdem flop hero Ax vs random hooked (bm slices)", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("2d 7c 9s"), boardMajor: true,
});
await checkHooked("holdem river hero Ax vs random hooked", {
  variant: "holdem", hero: cards("As"), heroDraw: 1, villain: [], villainDraw: 2,
  board: cards("Kd 7c 2s 9h 3d"),
});
// hooked xc with a multi-tile hero axis: chunks are cell ranges, so the
// snapshot cursor crosses hero-tile boundaries mid-board
await checkHooked("holdem flop both 2 drawn hooked (xc cells)", {
  variant: "holdem", hero: [], heroDraw: 2, villain: [], villainDraw: 2,
  board: cards("2d 7c 9s"),
});
await checkHooked("omaha hilo turn hero 1 drawn villain 2 drawn hooked (xc)", {
  variant: "omaha", himode: "hilo",
  hero: cards("As 2d Th"), heroDraw: 1, villain: cards("Ah 3c"), villainDraw: 2,
  board: cards("4s 5d 8c Kh"),
});
{
  let polls = 0, threw = null;
  try {
    await computeEquity({ variant: "holdem", hero: cards("As Ah"),
      villain: [], villainDraw: 2, board: [], cancelled: () => ++polls > 1 });
  } catch (e) { threw = e.message; }
  nrun++;
  if (threw === "cancelled") {
    console.log("ok cancellation: aborted at a chunk boundary");
  } else {
    nfail++;
    console.error(`FAIL cancellation: expected throw, got ${threw}`);
  }
}
// cancel inside the hero-draw inner loop (the headline both-unknown spot:
// holdem preflop Ax vs random, 1.1e11 deals)
{
  let polls = 0, threw = null;
  try {
    await computeEquity({ variant: "holdem", hero: cards("As"), heroDraw: 1,
      villain: [], villainDraw: 2, board: [], cancelled: () => ++polls > 2 });
  } catch (e) { threw = e.message; }
  nrun++;
  if (threw === "cancelled") {
    console.log("ok cancellation (hero loop): aborted at a chunk boundary");
  } else {
    nfail++;
    console.error(`FAIL cancellation (hero loop): expected throw, got ${threw}`);
  }
}
// past the 2^53 exact-tally guard (omaha random vs random): must refuse
{
  let threw = null;
  try {
    await computeEquity({ variant: "omaha", himode: "high", hero: [],
      heroDraw: 4, villain: [], villainDraw: 4, board: [] });
  } catch (e) { threw = e.message; }
  nrun++;
  if (threw && threw.includes("exceeds exact")) {
    console.log("ok 2^53 guard: omaha both-random refused");
  } else {
    nfail++;
    console.error(`FAIL 2^53 guard: expected refusal, got ${threw}`);
  }
}

if (FULL) {
  await check("holdem preflop AA vs KK (full 1.7M)", {
    variant: "holdem", hero: cards("As Ah"), villain: cards("Ks Kh"), board: [],
  });
  await check("holdem preflop AKs vs 76s (full 1.7M)", {
    variant: "holdem", hero: cards("Ah Kh"), villain: cards("7s 6s"), board: [],
  });
  await check("holdem flop vs random (894K naive)", {
    variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
    board: cards("2d 7c 9s"),
  });
  await checkDM("omaha4 flop vs random (122M)", {
    variant: "omaha", himode: "high",
    hero: cards("9s 8d 7h 6c"), villain: [], villainDraw: 4,
    board: cards("Ts Jd Ac"),
  });
  await checkDM("omaha4 hilo flop vs random (122M)", {
    variant: "omaha", himode: "hilo",
    hero: cards("As 2d Kh Qc"), villain: [], villainDraw: 4,
    board: cards("6s 7d Jc"),
  });
  // the phase-3 headline space: C(50,2)=1225 villain draws x C(48,5)=1.71M
  // boards = 2.1G deals in one call, checked against 1225 known-good calls
  await checkSum("holdem preflop AA vs random (full 2.1G, sum identity)", {
    variant: "holdem", hero: cards("As Ah"), villain: [], villainDraw: 2,
    board: [],
  });
}

console.log(nfail ? `${nfail}/${nrun} FAILED` : `all ${nrun} spots exact-match`);
Deno.exit(nfail ? 1 : 0);
