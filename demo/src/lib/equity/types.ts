// Types shared by the equity solver and its callers.

export type Variant = "holdem" | "omaha";
export type HiMode = "high" | "hilo";

/** high-only tally: every counter is a deal count. */
export interface RawHigh { win: number; tie: number; boards: number }
/** hi-lo tally: `quarters` is in quarter-pot units, the rest are deal counts. */
export interface RawHilo {
  quarters: number; boards: number; winH: number; tieH: number;
  heroLow: number; splitLow: number; noLow: number; scoop: number;
}
export type RawTally = RawHigh | RawHilo;

export interface EquityProgress {
  done: number;
  total: number;
  /** exact partial tally at this snapshot; absent on the initial done=0 tick */
  raw?: RawTally;
}

export interface EquityResult {
  hero: number;
  villain: number;
  boards: number;
  heroHands: number;
  villainHands: number;
  runouts: number;
  ms: number;
  exact: true;
  raw: RawTally;
}

export interface SpotShape {
  /** distinct known cards anywhere in the spot */
  known: number;
  boardLen: number;
  heroDraw?: number;
  villainDraw?: number;
}

export interface DealSpace {
  heroHands: number;
  villainHands: number;
  runouts: number;
  deals: number;
}

// Progress, cancellation and pause hooks a solve is driven with. Passing
// any of them switches the dispatch loop to bounded chunks with a tally
// snapshot after each; without them the whole grid is submitted up front
// and read back once.
export interface RunHooks {
  /** fires once with done=0 before the first dispatch, then with the exact
   *  partial tally at every chunk boundary */
  onProgress?: ((p: EquityProgress) => void) | null;
  /** polled at every chunk boundary; true aborts the run with an Error */
  cancelled?: (() => boolean) | null;
  /** polled at every chunk boundary; a returned promise parks the dispatch
   *  loop (already-queued chunks still drain) until it resolves */
  gate?: (() => Promise<void> | null) | null;
}

/** one (lo, hi) u32 plane-mask pair */
export type Mask = [number, number];
