import type { RawHigh, RawHilo, RawTally } from "./equity.ts";

// A tally oriented to the displayed hero, which is not always the kernel's.
export interface OrientedHigh {
  hilo: false;
  boards: number;
  win: number;
  tie: number;
  loss: number;
}

export interface OrientedHilo {
  hilo: true;
  boards: number;
  quarters: number;
  winH: number;
  tieH: number;
  lossH: number;
  heroLow: number;
  splitLow: number;
  villLow: number;
  noLow: number;
  scoop: number;
  /** which seat the scoop count belongs to (the kernel's hero) */
  scoopBy: "hero" | "villain";
}

export type Oriented = OrientedHigh | OrientedHilo;

// raw tallies count from the kernel-hero side; with `swap` (the displayed
// hero took the kernel's villain seat) every asymmetric counter flips.
// Scoops stay with the kernel-hero side; scoopBy names it.
export function orientRaw(raw: RawTally, hilo: boolean, swap: boolean): Oriented {
  const b = raw.boards;
  if (!hilo) {
    const r = raw as RawHigh;
    return {
      hilo: false,
      boards: b, tie: r.tie,
      win: swap ? b - r.win - r.tie : r.win,
      loss: swap ? r.win : b - r.win - r.tie,
    };
  }
  const r = raw as RawHilo;
  const villLow = b - r.heroLow - r.splitLow - r.noLow;
  return {
    hilo: true,
    boards: b,
    quarters: swap ? 4 * b - r.quarters : r.quarters,
    winH: swap ? b - r.winH - r.tieH : r.winH,
    tieH: r.tieH,
    lossH: swap ? r.winH : b - r.winH - r.tieH,
    heroLow: swap ? villLow : r.heroLow,
    splitLow: r.splitLow,
    villLow: swap ? r.heroLow : villLow,
    noLow: r.noLow,
    scoop: r.scoop,
    scoopBy: swap ? "villain" : "hero",
  };
}

// ---- showdown verdicts ----

export interface SeatChip { text: string; cls: string }

export interface Verdict {
  headline: string;
  cls: string;
  rows: Array<[tag: string, who: string, cls: string]>;
  hero: SeatChip;
  villain: SeatChip;
}

const SIDE_TEXT = { hero: "Hero", villain: "Villain", split: "Split" };
const SIDE_CLS = { hero: "win", villain: "loss", split: "tie" };

// [hero chip, villain chip] for the high-only outcome
const HIGH_CHIP: Record<string, [SeatChip, SeatChip]> = {
  hero:    [{ text: "WINNER", cls: "win" }, { text: "loses", cls: "loss" }],
  villain: [{ text: "loses", cls: "loss" }, { text: "WINNER", cls: "win" }],
  split:   [{ text: "SPLIT", cls: "tie" }, { text: "SPLIT", cls: "tie" }],
};

// indexed by the seat's own quarter-pots; villain's count is 4 - hero's
const QUARTER_CHIP: SeatChip[] = [
  { text: "loses", cls: "loss" }, { text: "¼ pot", cls: "loss" },
  { text: "SPLIT", cls: "tie" }, { text: "¾ pot", cls: "win" },
  { text: "SCOOPS", cls: "win" },
];

// `d` is the oriented tally of the single board
export function showdownVerdict(d: Oriented): Verdict {
  if (!d.hilo) {
    const who = d.win ? "hero" : d.tie ? "split" : "villain";
    const [hero, villain] = HIGH_CHIP[who];
    return {
      headline: who === "split" ? "Split pot" : `${SIDE_TEXT[who]} wins`,
      cls: SIDE_CLS[who], rows: [], hero, villain,
    };
  }
  const q = d.quarters; // hero's quarter-pots on this board, 0..4
  const high = d.winH ? "hero" : d.tieH ? "split" : "villain";
  // with no qualifying low the high hand takes both halves, so q is 4, 2 or 0
  const low = d.noLow ? "none" : d.heroLow ? "hero" : d.splitLow ? "split" : "villain";
  return {
    headline: q === 4 ? "Hero scoops" : q === 0 ? "Villain scoops"
      : q === 2 ? "Split pot" : `${q > 2 ? "Hero" : "Villain"} takes three quarters`,
    cls: q > 2 ? "win" : q < 2 ? "loss" : "tie",
    rows: [
      ["HIGH", SIDE_TEXT[high], SIDE_CLS[high]],
      low === "none"
        ? ["LOW", "no qualifying low — the high hand takes the whole pot", "none"]
        : ["LOW", SIDE_TEXT[low], SIDE_CLS[low]],
    ],
    hero: QUARTER_CHIP[q], villain: QUARTER_CHIP[4 - q],
  };
}
