import { dealSpace, type DealSpace, type HiMode, type SpotShape, type Variant }
  from "./equity.ts";
import type { TableState } from "./table.ts";

/** the whole table, in the form the equity engine and the panel both read */
export interface Spot {
  variant: Variant;
  himode: HiMode;
  /** himode === "hilo" and the variant supports it */
  hilo: boolean;
  heroKnown: number[];
  villKnown: number[];
  /** cards left unknown in each seat */
  heroU: number;
  villU: number;
  board: number[];
}

export function spotOf(
  table: TableState, variant: Variant, himode: HiMode,
): Spot {
  const heroKnown = table.hero.filter((c): c is number => c !== null);
  const villKnown = table.villain.filter((c): c is number => c !== null);
  return {
    variant,
    himode,
    hilo: variant !== "holdem" && himode === "hilo",
    heroKnown,
    villKnown,
    heroU: table.hero.length - heroKnown.length,
    villU: table.villain.length - villKnown.length,
    board: table.board.slice(),
  };
}

export const shapeOf = (spot: Spot): SpotShape => ({
  known: spot.heroKnown.length + spot.villKnown.length + spot.board.length,
  boardLen: spot.board.length,
  heroDraw: spot.heroU,
  villainDraw: spot.villU,
});

export const sizeOf = (spot: Spot): DealSpace => dealSpace(shapeOf(spot));

// every card face up: one deal, so the panel states the outcome instead of
// a probability
export const isShowdown = (spot: Spot) =>
  spot.heroU === 0 && spot.villU === 0 && spot.board.length === 5;

// changes exactly when the answer would; the engine re-seats on it
export const spotKey = (spot: Spot) =>
  [spot.variant, spot.himode, spot.heroU, spot.villU,
   spot.heroKnown.join("."), spot.villKnown.join("."), spot.board.join(".")].join("|");
