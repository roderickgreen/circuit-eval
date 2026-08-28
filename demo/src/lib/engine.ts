// The solve behind the table: which spots start by themselves, which wait for
// a click, which are refused outright, and what the panel says while one runs.
// React-free; hooks/useEquityEngine.ts subscribes to it.

import {
  computeEquity, heroShare, prewarmEquity, worklistSizes,
  type DealSpace,
} from "./equity.ts";
import { fmtDur, pctf } from "./format.ts";
import { estimate, noteRate } from "./rates.ts";
import { isShowdown, shapeOf, sizeOf, spotKey, type Spot } from "./spot.ts";
import { orientRaw, showdownVerdict, type Oriented, type SeatChip, type Verdict }
  from "./tally.ts";

// Spots at or under this many deals solve the instant the spot changes: every
// fully-known and one-unknown-card spot, sub-second on a phone. Anything
// bigger states its size and waits for a click.
const AUTO_MAX_DEALS = 1e8;

// Ceiling on the work list a spot would have to build before its first
// dispatch. Lists are enumerated and shuffled synchronously at 8 bytes an
// entry: 20M is ~160 MB and a few hundred ms of blocked tab. The next step
// up (eight-card Omaha against an unknown hand) is 177M entries, 1.4 GB and
// about a minute of dead tab, for a ~28-hour run.
const MAX_WORKLIST = 2e7;

// tick the seat chips + panel numbers from the exact partial tallies while
// a big spot enumerates; flip to false to show only the progress bar
const LIVE_TICK = true;

/** what the results panel is currently showing. One component, six states. */
export type PanelState =
  | { kind: "empty"; message: string }
  // "size": past exact tallies (`size` null) or past what a tab should
  // build; "gpu": no WebGPU, so every spot is refused
  | { kind: "refused"; why: "size" | "gpu"; hilo: boolean; deals: number;
      size: DealSpace | null }
  | { kind: "pending"; hilo: boolean; deals: number; estimate: string; size: DealSpace }
  | {
      kind: "running"; hilo: boolean; paused: boolean;
      done: number; total: number; elapsed: number; eta: string;
      frac: number; d: Oriented | null;
    }
  | {
      kind: "stopped"; hilo: boolean;
      done: number; total: number; elapsed: number; d: Oriented | null;
    }
  | { kind: "done"; hilo: boolean; ms: number; d: Oriented; size: DealSpace }
  | { kind: "showdown"; hilo: boolean; ms: number; d: Oriented; verdict: Verdict };

/** the two chips on the felt: a percentage each, or a decided outcome each */
export type SeatDisplay =
  | { kind: "pct"; hero: string; villain: string }
  | { kind: "verdict"; hero: SeatChip; villain: SeatChip };

/** everything the engine displays. Replaced wholesale, never mutated. */
export interface EngineState {
  panel: PanelState;
  seat: SeatDisplay | null;
  /** the line under the panel: compile waits and failures */
  status: string;
}

const NO_RESULT: PanelState = { kind: "empty", message: "no result" };

// ---- policy ----

/** what a freshly seated spot should do; every ceiling is decided here */
export type Plan =
  | { kind: "refused"; panel: PanelState }
  | { kind: "pending"; panel: PanelState; size: DealSpace }
  | { kind: "auto"; size: DealSpace };

export function planFor(spot: Spot): Plan {
  const size = sizeOf(spot);
  const deals = size.deals;

  // the float64 tally bound; there is no sampling fallback
  if (deals * (spot.hilo ? 4 : 1) > Number.MAX_SAFE_INTEGER) {
    return { kind: "refused",
             panel: { kind: "refused", why: "size", hilo: spot.hilo, deals, size: null } };
  }
  // a countable deal space can still need a work list too big to build
  if (worklistSizes(shapeOf(spot)).max > MAX_WORKLIST) {
    return { kind: "refused",
             panel: { kind: "refused", why: "size", hilo: spot.hilo, deals, size } };
  }
  if (deals <= AUTO_MAX_DEALS) return { kind: "auto", size };
  return {
    kind: "pending", size,
    panel: {
      kind: "pending", hilo: spot.hilo, deals,
      estimate: estimate(spot, deals), size,
    },
  };
}

// ---- one run ----

interface Deferred { promise: Promise<void>; resolve: () => void }

function deferred(): Deferred {
  let resolve!: () => void;
  const promise = new Promise<void>((r) => { resolve = r; });
  return { promise, resolve };
}

/**
 * A single solve and the transport state its dispatch loop polls between
 * chunks. Mutable: a pause has to be visible to a loop already awaiting.
 *
 * The instance is also the run's identity. The engine holds at most one as
 * `#active`, and every callback a run installs checks `#active === this`
 * before touching the panel, so a superseded run unwinds without effect.
 */
class Run {
  stop = false;
  paused: Deferred | null = null;
  pausedAt = 0;
  /** time spent parked, which is not time the GPU spent working */
  pausedMs = 0;
  /** repaint the progress panel from outside the dispatch loop (pause/resume) */
  redraw: (() => void) | null = null;
  readonly t0 = performance.now();

  // GPU time: parked time is held out, matching computeEquity's ms
  elapsed(): number {
    const parked = this.paused ? performance.now() - this.pausedAt : 0;
    return performance.now() - this.t0 - this.pausedMs - parked;
  }

  wake() {
    this.paused!.resolve();
    this.paused = null;
    this.pausedMs += performance.now() - this.pausedAt;
  }
}

interface Pending { spot: Spot; size: DealSpace }

// ---- the engine ----

/**
 * Owns the solve for whatever spot is on the table. Seat a spot with
 * `setSpot`, drive it with compute/pause/resume/stop, and read the display
 * through `subscribe` + `getState`.
 */
export class EquityEngine {
  #state: EngineState = { panel: NO_RESULT, seat: null, status: "" };
  #listeners = new Set<() => void>();

  /** the seated spot, as a key. Re-seating the same spot is a no-op. */
  #key: string | null = null;
  #active: Run | null = null;
  #pending: Pending | null = null;

  // ---- store ----

  getState = (): EngineState => this.#state;

  subscribe = (fn: () => void): (() => void) => {
    this.#listeners.add(fn);
    return () => { this.#listeners.delete(fn); };
  };

  #set(patch: Partial<EngineState>) {
    const keys = Object.keys(patch) as Array<keyof EngineState>;
    if (keys.every((k) => patch[k] === this.#state[k])) return;
    this.#state = { ...this.#state, ...patch };
    for (const fn of this.#listeners) fn();
  }

  // ---- seating ----

  /**
   * Put a spot on the table. Idempotent on the spot's key, so a re-render
   * that leaves the table alone never restarts a live solve.
   */
  setSpot(spot: Spot): void {
    const key = spotKey(spot);
    if (key === this.#key) return;
    this.#key = key;
    this.#supersede();
    this.#pending = null;

    if (!navigator.gpu) {
      const size = sizeOf(spot);
      this.#set({ seat: null, status: "", panel: {
        kind: "refused", why: "gpu", hilo: spot.hilo, deals: size.deals, size,
      } });
      return;
    }
    const plan = planFor(spot);
    if (plan.kind === "auto") {
      void this.#start(spot, plan.size);
      return;
    }
    if (plan.kind === "pending") this.#pending = { spot, size: plan.size };
    this.#set({ seat: null, status: "", panel: plan.panel });
  }

  /**
   * Drop the result and forget the seated spot. Seating is idempotent, so a
   * re-deal that lands on the spot already seated (reset on the opening
   * hand) would otherwise keep the finished answer with no way to run it
   * again; after clear, the next setSpot seats it from scratch.
   */
  clear = () => {
    this.#supersede();
    this.#pending = null;
    this.#key = null;
    this.#set({ panel: NO_RESULT, seat: null, status: "" });
  };

  /** for a view going away; listeners unsubscribe on their own */
  dispose = () => {
    this.#supersede();
    this.#pending = null;
    this.#key = null;
  };

  // ---- controls ----

  /** run the spot that is waiting for a click, if one is */
  compute = () => {
    const p = this.#pending;
    if (p) void this.#start(p.spot, p.size);
  };

  pause = () => {
    const r = this.#active;
    if (!r || r.paused) return;
    r.paused = deferred();
    r.pausedAt = performance.now();
    r.redraw?.();
  };

  resume = () => {
    const r = this.#active;
    if (!r?.paused) return;
    r.wake();
    r.redraw?.();
  };

  stop = () => {
    const r = this.#active;
    if (!r) return;
    r.stop = true;
    if (r.paused) r.wake(); // wake a paused loop so it sees the stop
  };

  // ---- running ----

  // Nulling `#active` is the supersession: every guard the run holds now
  // fails, so it unwinds without touching the panel.
  #supersede() {
    const r = this.#active;
    if (!r) return;
    this.#active = null;
    r.stop = true;
    // a paused run is parked on a promise nobody else holds; release it so
    // the loop wakes, sees itself dropped and frees its buffers
    r.paused?.resolve();
    r.paused = null;
  }

  async #start(spot: Spot, size: DealSpace) {
    const hilo = spot.hilo;
    // the kernels' villain seat is the wide axis and the hero seat the
    // narrow one, so the side with more unknown cards takes the villain
    // seat; the raw counts are flipped back for display (orientRaw)
    const swap = spot.heroU > spot.villU;

    const run = new Run();
    this.#supersede();
    this.#active = run;
    this.#pending = null;
    this.#set({ seat: { kind: "pct", hero: "…", villain: "…" }, status: "" });

    let d: Oriented | null = null;   // latest oriented tally
    let frac = 0;
    let done = 0;

    const drawProgress = () => {
      if (this.#active !== run) return;
      const elapsed = run.elapsed();
      // no ETA in the first second: the sample is mostly fixed startup cost.
      // Still shown while paused, since `elapsed` excludes parked time and
      // the figure means GPU work remaining.
      const eta = done > 0 && elapsed >= 1000
        ? `${fmtDur((size.deals - done) * (elapsed / done))} left` : "";
      this.#set({
        panel: {
          kind: "running", hilo, paused: !!run.paused,
          done, total: size.deals, elapsed, eta, frac, d,
        },
      });
    };
    run.redraw = drawProgress;

    try {
      // normally prewarmed at page load, so this resolves instantly; when
      // it doesn't, say what the wait is
      const timer = setTimeout(() => {
        if (this.#active === run) this.#set({ status: "compiling kernels…" });
      }, 150);
      try {
        await prewarmEquity({ variant: spot.variant, himode: spot.himode });
      } finally {
        clearTimeout(timer);
      }
      if (this.#active !== run) return;
      this.#set({ status: "" });

      const r = await computeEquity({
        variant: spot.variant,
        himode: spot.himode,
        hero: swap ? spot.villKnown : spot.heroKnown,
        villain: swap ? spot.heroKnown : spot.villKnown,
        heroDraw: swap ? spot.villU : spot.heroU,
        villainDraw: swap ? spot.heroU : spot.villU,
        board: spot.board,
        onProgress: (p) => {
          if (this.#active !== run) return;
          done = p.done;
          frac = p.done / p.total;
          // recorded per tick so a stopped run still contributes a rate
          noteRate(spot, done, performance.now() - run.t0 - run.pausedMs);
          d = LIVE_TICK && p.raw && p.raw.boards > 0
            ? orientRaw(p.raw, hilo, swap) : null;
          if (d) {
            const eq = heroShare(d);
            this.#set({ seat: { kind: "pct", hero: pctf(eq), villain: pctf(1 - eq) } });
          }
          drawProgress();
        },
        cancelled: () => this.#active !== run || run.stop,
        gate: () => run.paused?.promise ?? null,
      });
      if (this.#active !== run) return;
      this.#active = null;
      noteRate(spot, r.boards, r.ms);
      const od = orientRaw(r.raw, hilo, swap);
      if (isShowdown(spot)) {
        const v = showdownVerdict(od);
        this.#set({
          seat: { kind: "verdict", hero: v.hero, villain: v.villain },
          panel: { kind: "showdown", hilo, ms: r.ms, d: od, verdict: v },
        });
        return;
      }
      this.#set({
        seat: {
          kind: "pct",
          hero: pctf(swap ? r.villain : r.hero),
          villain: pctf(swap ? r.hero : r.villain),
        },
        panel: { kind: "done", hilo, ms: r.ms, d: od, size },
      });
    } catch (e) {
      if (this.#active !== run) return;
      this.#active = null;
      if (run.stop) {
        // partial tallies over a shuffled work list are a fair sample, so a
        // stopped run keeps what it counted
        this.#set({
          panel: {
            kind: "stopped", hilo, d,
            done, total: size.deals, elapsed: run.elapsed(),
          },
        });
        this.#pending = { spot, size };
        return;
      }
      this.#set({ seat: null, panel: NO_RESULT, status: `equity failed: ${(e as Error).message}` });
    }
  }
}
