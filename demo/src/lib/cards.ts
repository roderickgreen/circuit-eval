// Card vocabulary. Card id = 4*rank + suit, matching the circuit input
// convention (ENCODING.md); rank 0 = deuce .. 12 = ace. DOM-free.

export const RANKS = "23456789TJQKA";

// four-color deck
export const SUITS = [
  { ch: "♠", cls: "spades" },
  { ch: "♥", cls: "hearts" },
  { ch: "♦", cls: "diams" },
  { ch: "♣", cls: "clubs" },
] as const;

const SUIT_NAMES = ["spades", "hearts", "diamonds", "clubs"];
const RANK_NAMES = ["two", "three", "four", "five", "six", "seven", "eight",
                    "nine", "ten", "jack", "queen", "king", "ace"];

export const DECK_SIZE = 52;

/** a seat slot: a card id, or null for unknown (enumerated) */
export type Slot = number | null;

// spoken form, for aria-labels
export function cardName(id: number): string {
  return `${RANK_NAMES[id >> 2]} of ${SUIT_NAMES[id & 3]}`;
}

// "Ah" -> card id
export function cardId(s: string): number {
  const r = RANKS.indexOf(s[0].toUpperCase());
  const u = "shdc".indexOf(s[1].toLowerCase()); // matches SUITS order above
  if (r < 0 || u < 0) throw new Error(`bad card "${s}"`);
  return 4 * r + u;
}

export const rankChar = (id: number) => RANKS[id >> 2];
export const suitOf = (id: number) => SUITS[id & 3];

/** every card the spot has already committed, in any seat or on the board */
export function usedCards(hero: Slot[], villain: Slot[], board: number[]): Set<number> {
  const used = new Set<number>();
  for (const c of hero) if (c !== null) used.add(c);
  for (const c of villain) if (c !== null) used.add(c);
  for (const c of board) used.add(c);
  return used;
}

export function liveCards(used: Set<number>): number[] {
  const out: number[] = [];
  for (let c = 0; c < DECK_SIZE; c++) if (!used.has(c)) out.push(c);
  return out;
}

/** n cards drawn at random from whatever `used` leaves in the deck */
export function drawLive(used: Set<number>, n: number): number[] {
  const pool = liveCards(used);
  const out: number[] = [];
  for (let i = 0; i < n; i++) {
    out.push(...pool.splice(Math.floor(Math.random() * pool.length), 1));
  }
  return out;
}

// next street's board size, or null at the river (1-2 custom cards still
// complete to the flop)
export function nextStreetCount(boardLen: number): number | null {
  return boardLen < 3 ? 3 : boardLen < 5 ? boardLen + 1 : null;
}

export const STREET_NAME: Record<number, string> = { 3: "flop", 4: "turn", 5: "river" };
