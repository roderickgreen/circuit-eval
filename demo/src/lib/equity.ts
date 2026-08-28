// Exact heads-up equity via the enumeration kernels: every remaining runout
// is visited once, so the returned equities are exact counts, not
// estimates.
//
// The combinatorics live on the CPU: every spot builds work lists (board
// combos, villain-draw combos and, for cross-compare, hero-draw combos;
// worklist.ts) as packed plane-mask pairs, uploads them as storage
// buffers, and the kernels just walk them -- transpose masks to planes,
// evaluate the circuit, compare, tally. Pairs where two lists claim the
// same card are dropped in-kernel, so the counted deals are exactly
// C(nlive, h) * C(nlive - h, v) * C(nlive - h - v, k).
//
// The pieces, in the order a solve uses them:
//   equity/kernels.ts    which kernel a spot routes to; compile and cache
//   equity/uniform.ts    the kernels' uniform block, word by word
//   equity/dispatch.ts   submitting chunks, reading tallies back, hooks
//   equity/schedules.ts  the grid walks: how a deal space is chunked

import { getGPU } from "./gpu.ts";
import { comboMasks, shuffleMasks } from "./worklist.ts";
import { binom } from "./equity/combinatorics.ts";
import { chooseRoute, getKernel, kernelSet, planeMask,
         type Route } from "./equity/kernels.ts";
import { Dispatch, decodeTally } from "./equity/dispatch.ts";
import { walkBoardMajorChunked, walkCrossCompare, walkGrid,
         type Grid } from "./equity/schedules.ts";
import type { DealSpace, EquityResult, HiMode, Mask, RawTally, RunHooks,
              SpotShape, Variant } from "./equity/types.ts";

export type { DealSpace, EquityProgress, EquityResult, HiMode, RawHigh, RawHilo,
              RawTally, SpotShape, Variant } from "./equity/types.ts";

// Deal-space factorization for a spot, without touching the GPU: the UI
// needs the size *before* deciding whether to run (small spots go on their
// own, big ones wait for a click). deals = heroHands * villainHands *
// runouts, which is exactly what computeEquity will tally.
export function dealSpace(
  { known, boardLen, heroDraw = 0, villainDraw = 0 }: SpotShape,
): DealSpace {
  const nlive = 52 - known;
  const k = 5 - boardLen;
  const heroHands = binom(nlive, heroDraw);
  const villainHands = binom(nlive - heroDraw, villainDraw);
  const runouts = binom(nlive - heroDraw - villainDraw, k);
  return { heroHands, villainHands, runouts,
           deals: heroHands * villainHands * runouts };
}

// 8 bytes per entry: one (lo, hi) u32 plane-mask pair, matching the kernels'
// array<vec2<u32>> bindings.
export const WORKLIST_ENTRY_BYTES = 8;

// Lengths of the three work lists `computeEquity` is about to allocate. Each
// is enumerated over the *whole* live deck (the kernels drop the tuples that
// collide), so these are C(nlive, .) and not the reduced factors dealSpace
// reports. Building and shuffling a list is synchronous and unchunked, so a
// big one is a frozen tab and a large allocation before any GPU work
// starts; callers refuse those up front.
export function worklistSizes(
  { known, boardLen, heroDraw = 0, villainDraw = 0 }: SpotShape,
) {
  const nlive = 52 - known;
  const hero = binom(nlive, heroDraw);
  const villain = binom(nlive, villainDraw);
  const board = binom(nlive, 5 - boardLen);
  return { hero, villain, board, max: Math.max(hero, villain, board) };
}

// compile (and warm) the kernel set a variant/himode can route to ahead of
// the first solve. Idempotent and cheap once warm; prewarm.ts drives it
// across every variant at page load.
export async function prewarmEquity(
  { variant, himode }: { variant: Variant; himode: HiMode },
) {
  const { device } = await getGPU();
  const specs = Object.values(kernelSet(variant, himode));
  await Promise.all(specs.map((s) => getKernel(device, s)));
}

export interface EquityOptions extends RunHooks {
  variant: Variant;
  himode?: HiMode;
  /** known hero card ids */
  hero: readonly number[];
  /** known villain card ids */
  villain: readonly number[];
  /** revealed board cards only (0..5) */
  board: readonly number[];
  /** hero cards left unknown, enumerated over the live deck. The kernels
   *  evaluate hero once per dispatch, so each hero draw is a pass of the
   *  dispatch loop; callers should seat the side with more unknowns as the
   *  villain (equity is symmetric; flip the raw counts back afterwards) */
  heroDraw?: number;
  /** villain cards left unknown, enumerated per work-list entry: "hero vs
   *  random" is villainDraw=2 with an empty villain array */
  villainDraw?: number;
  /** force one kernel where several exist (cross-check harness; see
   *  verify/webgpu/perf_probe_bm.js). Exclusive. */
  dealMajor?: boolean;
  boardMajor?: boolean;
  xCompare?: boolean;
}

// Equities are in [0,1] (ties split; hi-lo in quarter-pot units). boards
// counts full deals (heroHands x villainHands x runouts). raw: high {win,
// tie, boards}; hi-lo adds the per-half breakdown {quarters, boards, winH,
// tieH, heroLow, splitLow, noLow, scoop}. Throws "cancelled" when the
// cancel hook fires.
export async function computeEquity(opts: EquityOptions): Promise<EquityResult> {
  const { variant, himode, hero, villain, board,
          heroDraw = 0, villainDraw = 0 } = opts;
  const used = distinctCards(hero, villain, board);
  const shape: SpotShape = { known: used.size, boardLen: board.length,
                             heroDraw, villainDraw };
  const route = chooseRoute(forcedRoute(opts), shape);

  const { device, adapter } = await getGPU();
  const K = await getKernel(device, kernelSet(variant, himode)[route]);
  const space = dealSpace(shape);
  assertSolvable(shape, space, device, K.hilo);

  const lists = buildWorkLists(used, shape, K.planeOfCard);
  const buffers = uploadWorkLists(device, lists, route === "crossCompare");
  const grid: Grid = {
    known: { hero: planeMask(hero, K.planeOfCard),
             villain: planeMask(villain, K.planeOfCard),
             board: planeMask(board, K.planeOfCard) },
    nb: lists.board.length / 2,
    nv: lists.villain.length / 2,
    nh: lists.hero.length / 2,
    heroDraws: maskPairs(lists.hero),
    deals: space.deals,
  };
  const run = new Dispatch(
    device, K, K.bindWith(buffers.boards, buffers.villains, buffers.heroes),
    adapter.limits.maxComputeWorkgroupsPerDimension, space.deals, opts);

  if (route === "crossCompare") await walkCrossCompare(run, grid, K.ht);
  else if (route === "boardMajor" && run.hooked) await walkBoardMajorChunked(run, grid);
  else await walkGrid(run, grid, route === "boardMajor");

  const { tally, ms } = await run.finish();
  buffers.destroy();
  if (run.aborted) throw new Error("cancelled");
  if (!tally) throw new Error("no tally read back");

  const raw = decodeTally(tally, K.hilo);
  if (raw.boards !== space.deals) {
    throw new Error(`tally ${raw.boards} != deals ${space.deals}`);
  }
  const heroEq = heroShare(raw);
  return { hero: heroEq, villain: 1 - heroEq, boards: raw.boards,
           heroHands: space.heroHands, villainHands: space.villainHands,
           runouts: space.runouts, ms, exact: true, raw };
}

// The hero's share of the pot implied by a tally, partial or final: wins plus
// half the ties for high, quarter-pots for hi-lo. One definition, because the
// live ticks and the final result must agree on what the seat chip means.
export const heroShare = (raw: RawTally): number =>
  "quarters" in raw
    ? raw.quarters / (4 * raw.boards)
    : (raw.win + raw.tie / 2) / raw.boards;

function distinctCards(...groups: (readonly number[])[]): Set<number> {
  const used = new Set(groups.flat());
  if (used.size !== groups.reduce((n, g) => n + g.length, 0)) {
    throw new Error("duplicate cards in spot");
  }
  return used;
}

function forcedRoute(
  { dealMajor = false, boardMajor = false, xCompare = false }: EquityOptions,
): Route | null {
  if (+dealMajor + +boardMajor + +xCompare > 1) {
    throw new Error("dealMajor, boardMajor and xCompare are exclusive");
  }
  return dealMajor ? "dealMajor" : boardMajor ? "boardMajor"
       : xCompare ? "crossCompare" : null;
}

// Refuse a spot before anything is built: the largest work list must fit a
// storage binding, and the final tally must stay an exact float64 (tallies
// are u64 on the GPU; hi-lo counts quarters, up to 4 per deal).
function assertSolvable(
  shape: SpotShape, space: DealSpace, device: GPUDevice, hilo: boolean,
): void {
  const wl = worklistSizes(shape);
  const maxBind = device.limits.maxStorageBufferBindingSize;
  if (wl.max * WORKLIST_ENTRY_BYTES > maxBind) {
    throw new Error(`work list of ${wl.max} entries exceeds the device's `
      + `${maxBind}-byte storage binding limit`);
  }
  if (space.deals * (hilo ? 4 : 1) > Number.MAX_SAFE_INTEGER) {
    throw new Error(`${space.deals.toExponential(2)} deals exceeds exact 2^53 tallies`);
  }
}

interface WorkLists {
  board: Uint32Array<ArrayBuffer>;
  villain: Uint32Array<ArrayBuffer>;
  hero: Uint32Array<ArrayBuffer>;
}

// The three lists, each enumerated over the whole live deck (the kernels
// drop tuples sharing a card) and shuffled so any prefix of the run is a
// fair sample of the space. Fixed distinct seeds keep the order
// reproducible per spot and the lists uncorrelated. A k=0 / v=0 / h=0 list
// holds the one empty combo, so no list is ever zero-sized.
function buildWorkLists(
  used: Set<number>,
  { boardLen, heroDraw = 0, villainDraw = 0 }: SpotShape,
  planeOfCard: number[],
): WorkLists {
  const live: number[] = [];
  for (let c = 0; c < 52; c++) if (!used.has(c)) live.push(planeOfCard[c]);
  return {
    board: shuffleMasks(comboMasks(live, 5 - boardLen), 0x9e3779b9),
    villain: shuffleMasks(comboMasks(live, villainDraw), 0x85ebca6b),
    hero: shuffleMasks(comboMasks(live, heroDraw), 0xc2b2ae35),
  };
}

function maskPairs(masks: Uint32Array): Mask[] {
  const pairs: Mask[] = [];
  for (let i = 0; i < masks.length; i += 2) pairs.push([masks[i], masks[i + 1]]);
  return pairs;
}

// per-solve storage buffers; the hero list is bound by cross-compare
// kernels only. destroy() once the last readback has drained.
function uploadWorkLists(device: GPUDevice, lists: WorkLists, withHeroes: boolean) {
  const upload = (masks: Uint32Array<ArrayBuffer>) => {
    const buf = device.createBuffer({
      size: masks.byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(buf, 0, masks);
    return buf;
  };
  const boards = upload(lists.board);
  const villains = upload(lists.villain);
  const heroes = withHeroes ? upload(lists.hero) : null;
  return {
    boards, villains, heroes,
    destroy() { boards.destroy(); villains.destroy(); heroes?.destroy(); },
  };
}
