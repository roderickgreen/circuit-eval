import { fmtDur } from "./format.ts";
import type { Spot } from "./spot.ts";

// Peak measured throughput (deals/ms) per kernel route, for estimating how
// long an un-run spot would take. Only runs past RATE_MIN_DEALS count (small
// solves are mostly fixed cost), and each route keeps its own figure since
// they differ by an order of magnitude per deal.
const RATE_MIN_DEALS = 1e8;

// module state: a property of the machine, kept across remounts
const rates = new Map<string, number>();

// mirrors chooseRoute (equity/kernels.ts) closely enough to price a spot
function routeKey(spot: Spot): string {
  const route = spot.heroU >= 1 && spot.villU >= 1 ? "xc" : "fused";
  return `${spot.variant}:${spot.himode}:${route}`;
}

export function noteRate(spot: Spot, deals: number, ms: number) {
  if (deals < RATE_MIN_DEALS || !(ms > 0)) return;
  const k = routeKey(spot);
  rates.set(k, Math.max(rates.get(k) ?? 0, deals / ms));
}

export function estimate(spot: Spot, deals: number): string {
  const r = rates.get(routeKey(spot));
  return r ? `about ${fmtDur(deals / r)}` : "";
}
