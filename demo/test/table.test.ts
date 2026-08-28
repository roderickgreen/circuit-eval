// The felt's edit rules, with no renderer involved: every transition the
// table offers is a pure function in src/lib/table.ts, and this is where
// those rules are pinned down.

import { describe, expect, it } from "vitest";

import { cardId, DECK_SIZE, type Slot } from "../src/lib/cards.ts";
import {
  clearBoard, clearSeat, clearSlot, dealStreet, openingTable, placeCard, randSeat,
  slotCard, tableUsed, type TableState,
} from "../src/lib/table.ts";

const ids = (s: string) => s.split(" ").map(cardId);
const sorted = (xs: Array<number | null>) =>
  [...xs].sort((a, b) => (a ?? -1) - (b ?? -1));
const t0: TableState = {
  hero: ids("Ah Ad"),
  villain: [cardId("Kh"), null],
  board: ids("2c 7d 9s"),
};

describe("slots", () => {
  it("reads a seat slot's card", () => {
    expect(slotCard(t0, "hero", 0)).toBe(cardId("Ah"));
  });
  it("reads an unknown seat slot as null", () => {
    expect(slotCard(t0, "villain", 1)).toBeNull();
  });
  it("reads a dealt board slot's card", () => {
    expect(slotCard(t0, "board", 2)).toBe(cardId("9s"));
  });
  it("reads a board slot past the prefix as null", () => {
    expect(slotCard(t0, "board", 3)).toBeNull();
  });

  it("clears only the named seat slot", () => {
    expect(clearSlot(t0, "hero", 1).hero).toEqual([cardId("Ah"), null]);
  });
  it("un-deals a cleared board slot", () => {
    expect(clearSlot(t0, "board", 1).board).toEqual(ids("2c 9s"));
  });

  it("places into a seat slot at its index", () => {
    expect(placeCard(t0, "villain", 1, cardId("Kd")).villain).toEqual(ids("Kh Kd"));
  });
  it("appends a board pick whatever the index", () => {
    expect(placeCard(t0, "board", 4, cardId("Th")).board).toEqual(ids("2c 7d 9s Th"));
  });

  it("leaves the old table alone", () => {
    clearSlot(t0, "hero", 0);
    placeCard(t0, "board", 3, cardId("Th"));
    expect(t0.hero).toEqual(ids("Ah Ad"));
    expect(t0.board).toEqual(ids("2c 7d 9s"));
  });
});

describe("whole seats", () => {
  it("clears a seat to all-unknown", () => {
    expect(clearSeat(t0, "hero").hero).toEqual([null, null]);
  });

  // rand releases the seat's own cards before drawing: with the villain
  // holding all 50 other cards, the only legal draw is the hero's own hand
  it("releases the seat's own cards before a rand draw", () => {
    const others: number[] = [];
    for (let c = 0; c < DECK_SIZE; c++) {
      if (c !== cardId("Ah") && c !== cardId("Ad")) others.push(c);
    }
    const drawn = randSeat({ hero: ids("Ah Ad"), villain: others, board: [] }, "hero");
    expect(sorted(drawn.hero)).toEqual(sorted(ids("Ah Ad")));
  });

  it("rand-fills every slot without touching the rest of the table", () => {
    const drawn = randSeat(t0, "villain").villain;
    const used = new Set<Slot>([...t0.hero, ...t0.board]);
    expect(drawn).toHaveLength(2);
    expect(drawn.every((c) => c !== null)).toBe(true);
    expect(drawn.some((c) => used.has(c))).toBe(false);
  });
});

describe("the board", () => {
  it("deals a flop from an empty board", () => {
    expect(dealStreet({ ...t0, board: [] }).board).toHaveLength(3);
  });
  it("completes a part-dealt board to the flop", () => {
    const dealt = dealStreet({ ...t0, board: ids("2c") }).board;
    expect(dealt).toHaveLength(3);
    expect(dealt.slice(0, 1)).toEqual(ids("2c"));
  });
  it("deals the turn after the flop", () => {
    expect(dealStreet(t0).board).toHaveLength(4);
  });
  it("leaves a complete board alone", () => {
    const river = { ...t0, board: ids("2c 7d 9s Th Jc") };
    expect(dealStreet(river).board).toEqual(river.board);
  });
  it("keeps dealt streets off the seats' cards", () => {
    const dealt = dealStreet(t0).board;
    expect(dealt.some((c) => t0.hero.includes(c) || t0.villain.includes(c)))
      .toBe(false);
  });

  it("clears back to preflop, seats untouched", () => {
    const cleared = clearBoard(t0);
    expect(cleared.board).toEqual([]);
    expect(cleared.hero).toEqual(t0.hero);
    expect(cleared.villain).toEqual(t0.villain);
  });
  it("clears an already-empty board to itself", () => {
    expect(clearBoard({ ...t0, board: [] }).board).toEqual([]);
  });

  it("counts every committed card as used, unknowns aside", () => {
    expect(sorted([...tableUsed(t0)])).toEqual(sorted(ids("Ah Ad Kh 2c 7d 9s")));
  });
});

describe("the opening table", () => {
  it("opens hold'em on the hand everybody has a number for", () => {
    expect(openingTable("holdem", 2)).toEqual(
      { hero: ids("Ah Ad"), villain: [null, null], board: [] });
  });
  it("opens omaha with hero known and villain's last two unknown", () => {
    const o = openingTable("omaha", 4);
    expect(o.hero.every((c) => c !== null)).toBe(true);
    expect(o.villain.map((c) => c === null)).toEqual([false, false, true, true]);
  });
  // the two ladders share no card at any length — worth checking every hole
  // count, since both draw spades from their first card and only the ranks
  // keep them apart
  it("never deals an opening card twice", () => {
    for (const n of [4, 5, 6, 7, 8]) {
      const o = openingTable("omaha", n);
      expect(tableUsed(o).size).toBe(o.hero.length + (n - 2));
    }
  });
});
