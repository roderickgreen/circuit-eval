// A spot in a URL. The fragment carries the whole table (variant, hole
// count, hi/lo, every card) and no result: the recipient's GPU computes the
// answer under the same rules as their own deals.
//
// The payload is a bit-packed byte string in base64url: ten header bits,
// then six bits per slot (0..51 a card, 52 unknown). Hold'em on the river
// is 8 bytes; eight-card Omaha, the widest spot the felt holds, is 17.

import { DECK_SIZE, type Slot } from "./cards.ts";
import { DEFAULTS, holeCountOf, loadSettings, type Settings } from "./settings.ts";
import { openingTable, type TableState } from "./table.ts";

/** the 6-bit slot value for an unknown card; 53..63 are unused */
const UNKNOWN = DECK_SIZE;
const SLOT_BITS = 6;

// version(2) variant(1) hilo(1) holeCards-2(3) boardLen(3)
const HEADER_BITS = 10;
const VERSION = 0;

/** the fragment parameter the payload rides in */
const PARAM = "s";

/** what a link restores */
export interface SharedSpot {
  settings: Settings;
  table: TableState;
}

// ---- bit stream: MSB-first, a bit at a time ----

class BitWriter {
  #out: number[] = [];
  #at = 0;

  put(value: number, width: number) {
    for (let i = width - 1; i >= 0; i--) {
      if ((this.#at >> 3) === this.#out.length) this.#out.push(0);
      this.#out[this.#at >> 3] |= ((value >> i) & 1) << (7 - (this.#at & 7));
      this.#at++;
    }
  }

  bytes = () => Uint8Array.from(this.#out);
}

class BitReader {
  #at = 0;
  constructor(private readonly src: Uint8Array) {}

  /** the next `width` bits; reading past the end yields zeroes, never NaN */
  take(width: number): number {
    let v = 0;
    for (let i = 0; i < width; i++, this.#at++) {
      v = (v << 1) | (((this.src[this.#at >> 3] ?? 0) >> (7 - (this.#at & 7))) & 1);
    }
    return v;
  }
}

const byteLen = (slots: number) =>
  Math.ceil((HEADER_BITS + SLOT_BITS * slots) / 8);

// ---- base64url ----

const toB64url = (b: Uint8Array) =>
  btoa(String.fromCharCode(...b))
    .replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");

function fromB64url(s: string): Uint8Array | null {
  if (!/^[A-Za-z0-9_-]+$/.test(s)) return null;
  try {
    const bin = atob(s.replace(/-/g, "+").replace(/_/g, "/"));
    return Uint8Array.from(bin, (c) => c.charCodeAt(0));
  } catch {
    return null; // a length atob refuses
  }
}

// ---- encode / decode ----

/** the payload for a spot */
export function encodeSpot(settings: Settings, table: TableState): string {
  const w = new BitWriter();
  w.put(VERSION, 2);
  w.put(settings.variant === "omaha" ? 1 : 0, 1);
  w.put(settings.himode === "hilo" ? 1 : 0, 1);
  w.put(table.hero.length - 2, 3);
  w.put(table.board.length, 3);
  for (const c of table.hero) w.put(c ?? UNKNOWN, SLOT_BITS);
  for (const c of table.villain) w.put(c ?? UNKNOWN, SLOT_BITS);
  for (const c of table.board) w.put(c, SLOT_BITS);
  return toB64url(w.bytes());
}

/**
 * The spot a payload describes, or null. Strict: the length must match the
 * shape the header claims, every card must be a real card, no card may
 * appear twice, and hold'em may not carry a hole count or hi-lo flag.
 */
export function decodeSpot(payload: string): SharedSpot | null {
  const bytes = fromB64url(payload);
  if (!bytes) return null;

  const r = new BitReader(bytes);
  if (r.take(2) !== VERSION) return null;
  const omaha = r.take(1) === 1;
  const hilo = r.take(1) === 1;
  const n = r.take(3) + 2;          // 2..9
  const boardLen = r.take(3);       // 0..7
  if (boardLen > 5) return null;
  if (omaha ? n > 8 : (n !== 2 || hilo)) return null;
  if (bytes.length !== byteLen(2 * n + boardLen)) return null;

  let bad = false;
  const used = new Set<number>();
  const slot = (): Slot => {
    const v = r.take(SLOT_BITS);
    if (v === UNKNOWN) return null;
    if (v > UNKNOWN || used.has(v)) { bad = true; return null; }
    used.add(v);
    return v;
  };

  const hero = Array.from({ length: n }, slot);
  const villain = Array.from({ length: n }, slot);
  const board: number[] = [];
  for (let i = 0; i < boardLen; i++) {
    const c = slot();
    // the board holds dealt cards only
    if (c === null) bad = true;
    else board.push(c);
  }
  if (bad) return null;

  return {
    // hold'em keeps the default hole count, so a later switch to Omaha
    // lands on four cards
    settings: omaha
      ? { variant: "omaha", holeCards: n, himode: hilo ? "hilo" : "high" }
      : { ...DEFAULTS },
    table: { hero, villain, board },
  };
}

// ---- the URL ----

/** the fragment for a spot, leading "#" included */
export const spotHash = (settings: Settings, table: TableState) =>
  `#${PARAM}=${encodeSpot(settings, table)}`;

/** the spot a fragment carries, if it carries one */
export function spotFromHash(hash: string): SharedSpot | null {
  const payload = new URLSearchParams(hash.replace(/^#/, "")).get(PARAM);
  return payload ? decodeSpot(payload) : null;
}

/** the table the page opens on: a link's spot, else saved settings and
 *  that game's opening hand */
export function openingState(): SharedSpot {
  const shared = spotFromHash(location.hash);
  if (shared) return shared;
  const settings = loadSettings();
  return { settings, table: openingTable(settings.variant, holeCountOf(settings)) };
}
