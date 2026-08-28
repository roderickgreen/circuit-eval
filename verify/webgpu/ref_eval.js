// Naive reference evaluators for validating the GPU equity kernels.
// Written from the rules, sharing nothing with the circuits: only the
// *ordering* of hands matters here, so scores are plain comparable ints.
// Card id = 4*rank + suit, rank 0 = deuce .. 12 = ace.

const rankOf = (c) => c >> 2;

// 5-card score: category * 13^5 + kickers base-13, higher = better.
export function score5(cards) {
  const rs = cards.map(rankOf).sort((a, b) => b - a);
  const suits = cards.map((c) => c & 3);
  const flush = suits.every((s) => s === suits[0]);
  const count = new Map();
  for (const r of rs) count.set(r, (count.get(r) ?? 0) + 1);
  // group by count desc, then rank desc — the kicker order of every category
  const groups = [...count.entries()].sort((a, b) => b[1] - a[1] || b[0] - a[0]);
  const kick = [];
  for (const [r, n] of groups) for (let i = 0; i < n; i++) kick.push(r);
  const shape = groups.map(([, n]) => n).join("");

  let straightHigh = -1;
  if (count.size === 5) {
    if (rs[0] - rs[4] === 4) straightHigh = rs[0];
    else if (rs[0] === 12 && rs[1] === 3) straightHigh = 3; // wheel A2345
  }
  let cat;
  if (straightHigh >= 0 && flush) cat = 9;
  else if (shape === "41") cat = 8;
  else if (shape === "32") cat = 7;
  else if (flush) cat = 6;
  else if (straightHigh >= 0) cat = 5;
  else if (shape === "311") cat = 4;
  else if (shape === "221") cat = 3;
  else if (shape === "2111") cat = 2;
  else cat = 1;

  let t;
  if (cat === 5 || cat === 9) t = [straightHigh, 0, 0, 0, 0];
  else t = kick;
  let v = cat;
  for (let i = 0; i < 5; i++) v = v * 13 + t[i];
  return v;
}

export function* combos(arr, k, start = 0, acc = []) {
  if (acc.length === k) { yield acc.slice(); return; }
  for (let i = start; i <= arr.length - (k - acc.length); i++) {
    acc.push(arr[i]);
    yield* combos(arr, k, i + 1, acc);
    acc.pop();
  }
}

// holdem: best 5 of (hole + board), any mix
export function scoreHoldem(hole, board) {
  let best = 0;
  for (const c of combos([...hole, ...board], 5)) {
    const s = score5(c);
    if (s > best) best = s;
  }
  return best;
}

// omaha: exactly 2 from hole, exactly 3 from board
export function scoreOmahaHigh(hole, board) {
  let best = 0;
  for (const h of combos(hole, 2)) {
    for (const b of combos(board, 3)) {
      const s = score5([...h, ...b]);
      if (s > best) best = s;
    }
  }
  return best;
}

// omaha 8-or-better low: 2 distinct low ranks from hole + 3 from board,
// all 5 distinct. Returns the circuit's own encoding (bit i = low rank i
// used, A=0..8=7; 0xff = no low; lower = better) — reimplemented from the
// rules, comparison-compatible by construction.
export function scoreOmahaLow(hole, board) {
  const lowIdx = (c) => {
    const r = rankOf(c);
    return r === 12 ? 0 : r <= 6 ? r + 1 : -1; // A=0, 2=1 .. 8=7
  };
  const present = (cards) => {
    const set = new Set();
    for (const c of cards) { const i = lowIdx(c); if (i >= 0) set.add(i); }
    return [...set];
  };
  const hl = present(hole), bl = present(board);
  let best = 0xff;
  for (const h of combos(hl, 2)) {
    for (const b of combos(bl, 3)) {
      const all = new Set([...h, ...b]);
      if (all.size !== 5) continue;
      let mask = 0;
      for (const i of all) mask |= 1 << i;
      if (mask < best) best = mask;
    }
  }
  return best;
}

// exact reference equity by full enumeration of the remaining board;
// heroDraw / villainDraw > 0 additionally enumerate every completion of a
// partially (or fully) unknown hand on that side (both at once nests the
// two sums). Tallies match the GPU kernel units.
export function refEquity(spot) {
  const { heroDraw = 0, villainDraw = 0 } = spot;
  if (!heroDraw && !villainDraw) return refEquityKnown(spot);
  const { hero, villain, board } = spot;
  const used = new Set([...hero, ...villain, ...board]);
  const live = [];
  for (let c = 0; c < 52; c++) if (!used.has(c)) live.push(c);
  const tot = {};
  const [side, n] = heroDraw ? ["hero", heroDraw] : ["villain", villainDraw];
  for (const draw of combos(live, n)) {
    const r = refEquity({ ...spot, [side]: [...spot[side], ...draw],
      [side === "hero" ? "heroDraw" : "villainDraw"]: 0 });
    for (const k of Object.keys(r)) tot[k] = (tot[k] ?? 0) + r[k];
  }
  return tot;
}

function refEquityKnown({ variant, himode, hero, villain, board }) {
  const used = new Set([...hero, ...villain, ...board]);
  const live = [];
  for (let c = 0; c < 52; c++) if (!used.has(c)) live.push(c);
  const k = 5 - board.length;
  const hilo = himode === "hilo";
  const scoreHigh = variant === "holdem" ? scoreHoldem : scoreOmahaHigh;

  // hi-lo carries the per-half breakdown the UI shows (same u64 counters
  // the kernels tally): high win/tie, low hero-best/split/none, and scoops
  // (hero takes all four quarters). Key order must match decodeTally in demo/src/lib/equity/dispatch.ts.
  let win = 0, tie = 0, boards = 0, quarters = 0;
  let heroLow = 0, splitLow = 0, noLows = 0, scoop = 0;
  for (const draw of combos(live, k)) {
    const full = [...board, ...draw];
    const sh = scoreHigh(hero, full);
    const sv = scoreHigh(villain, full);
    boards++;
    const qH = sh > sv ? 2 : sh === sv ? 1 : 0;
    if (sh > sv) win++;
    else if (sh === sv) tie++;
    if (hilo) {
      const lh = scoreOmahaLow(hero, full);
      const lv = scoreOmahaLow(villain, full);
      const noLow = lh === 0xff && lv === 0xff;
      const qL = noLow ? qH : lh < lv ? 2 : lh === lv ? 1 : 0;
      quarters += qH + qL;
      if (lh < lv) heroLow++;
      else if (lh === lv && !noLow) splitLow++;
      else if (noLow) noLows++;
      if (sh > sv && (noLow || lh < lv)) scoop++;
    }
  }
  return hilo
    ? { quarters, boards, winH: win, tieH: tie, heroLow, splitLow,
        noLow: noLows, scoop }
    : { win, tie, boards };
}
