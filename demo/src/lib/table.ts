// The table as the visitor edits it: two seats of slots (a card id, or null
// for unknown) and the board as an ordered prefix of dealt cards. Every edit
// is a pure TableState -> TableState function; App.tsx binds them to clicks.

import {
  cardId, drawLive, nextStreetCount, usedCards, type Slot,
} from "./cards.ts";
import type { Variant } from "./equity.ts";

export interface TableState {
  hero: Slot[];
  villain: Slot[];
  board: number[];
}

/** a clickable region of the felt */
export type Zone = "hero" | "villain" | "board";
export type SeatZone = "hero" | "villain";

const withSeat = (t: TableState, zone: SeatZone, next: Slot[]): TableState =>
  zone === "hero" ? { ...t, hero: next } : { ...t, villain: next };

/** every card the table has committed, in either seat or on the board */
export const tableUsed = (t: TableState): Set<number> =>
  usedCards(t.hero, t.villain, t.board);

/** the card in a slot; board slots past the dealt prefix read as null */
export function slotCard(t: TableState, zone: Zone, i: number): Slot {
  if (zone === "board") return i < t.board.length ? t.board[i] : null;
  return t[zone][i];
}

/** forget one slot: a seat card goes back to unknown, a board card is un-dealt */
export function clearSlot(t: TableState, zone: Zone, i: number): TableState {
  if (zone === "board") return { ...t, board: t.board.filter((_, j) => j !== i) };
  return withSeat(t, zone, t[zone].map((c, j) => (j === i ? null : c)));
}

// The board is an ordered prefix, so a board pick appends; the index is
// there to mirror clearSlot.
export function placeCard(
  t: TableState, zone: Zone, i: number, card: number,
): TableState {
  if (zone === "board") return { ...t, board: [...t.board, card] };
  return withSeat(t, zone, t[zone].map((c, j) => (j === i ? card : c)));
}

// clear makes every slot unknown; rand makes every slot a known random card
// (not "fill in the gaps")
export function clearSeat(t: TableState, zone: SeatZone): TableState {
  return withSeat(t, zone, Array<Slot>(t[zone].length).fill(null));
}

export function randSeat(t: TableState, zone: SeatZone): TableState {
  // this seat's own cards are back in the pool
  const other = zone === "hero" ? t.villain : t.hero;
  return withSeat(t, zone, drawLive(usedCards(other, [], t.board), t[zone].length));
}

export const clearBoard = (t: TableState): TableState => ({ ...t, board: [] });

/** complete the board to the next street; identity at the river */
export function dealStreet(t: TableState): TableState {
  const target = nextStreetCount(t.board.length);
  if (target === null) return t;
  return { ...t, board: [...t.board, ...drawLive(tableUsed(t), target - t.board.length)] };
}

// ---- opening table ----
//
// The same shape at every variant and hole count: hero fully known,
// villain's last two cards unknown, no board. Two unknowns puts the deal
// space near 10^9 whatever the hole count (2.10 billion at hold'em, 265
// million at eight-card omaha): a Compute click and a sub-second answer.
// One unknown is ~10^7, three passes 10^10.
//
// The ladders extend a card at a time and never collide: hero takes the top
// ranks, villain the ranks below. Villain's string runs six, the most it
// ever shows (eight cards less the two unknowns).
const OPENING = {
  hero: { ranks: "AAKKQQJJ", suits: "hshshshs" },
  villain: { ranks: "987654", suits: "ssddss" },
};
const OPENING_UNKNOWN = 2;

export function openingTable(variant: Variant, n: number): TableState {
  const ladder = (who: "hero" | "villain", k: number) =>
    Array.from({ length: k }, (_, i) =>
      cardId(OPENING[who].ranks[i] + OPENING[who].suits[i]));
  const shown = Math.max(0, n - OPENING_UNKNOWN);
  return {
    // hold'em opens on the hand with the best-known number
    hero: variant === "holdem" ? [cardId("Ah"), cardId("Ad")] : ladder("hero", n),
    villain: [...ladder("villain", shown), ...Array<Slot>(n - shown).fill(null)],
    board: [],
  };
}
