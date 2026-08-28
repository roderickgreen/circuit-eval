// The spot-in-a-URL round trip, with no renderer involved: src/lib/share.ts
// packs a table into a fragment and unpacks a stranger's fragment back into
// one, and this is where both halves are pinned down.

import { describe, expect, it } from "vitest";

import { cardId } from "../src/lib/cards.ts";
import {
  decodeSpot, encodeSpot, spotFromHash, spotHash, type SharedSpot,
} from "../src/lib/share.ts";
import { DEFAULTS, type Settings } from "../src/lib/settings.ts";
import { openingTable, type TableState } from "../src/lib/table.ts";

const ids = (s: string) => (s ? s.split(" ").map(cardId) : []);
const omaha = (holeCards: number, hilo = false): Settings =>
  ({ variant: "omaha", holeCards, himode: hilo ? "hilo" : "high" });

const round = (settings: Settings, table: TableState) =>
  decodeSpot(encodeSpot(settings, table));

describe("round trip", () => {
  it("carries a hold'em spot", () => {
    const table: TableState = {
      hero: ids("Ah Ad"), villain: ids("Ks Kc"), board: ids("2c 7d 9s"),
    };
    expect(round(DEFAULTS, table)).toEqual({ settings: DEFAULTS, table });
  });

  it("carries unknown seat slots as unknown", () => {
    const table: TableState = {
      hero: [cardId("Ah"), null], villain: [null, null], board: [],
    };
    expect(round(DEFAULTS, table)?.table).toEqual(table);
  });

  it("carries the hole count and the hi-lo flag", () => {
    const s = omaha(5, true);
    const got = round(s, openingTable("omaha", 5));
    expect(got?.settings).toEqual(s);
    expect(got?.table).toEqual(openingTable("omaha", 5));
  });

  it("carries every opening table the felt can hold", () => {
    for (const n of [2, 3, 4, 5, 6, 7, 8]) {
      for (const hilo of [false, true]) {
        const s = omaha(n, hilo);
        expect(round(s, openingTable("omaha", n))).toEqual(
          { settings: s, table: openingTable("omaha", n) });
      }
    }
  });

  it("carries a full board and a full eight-card table", () => {
    const table: TableState = {
      hero: ids("Ah As Kh Ks Qh Qs Jh Js"),
      villain: ids("Ad Ac Kd Kc Qd Qc Jd Jc"),
      board: ids("2c 7d 9s Th 4d"),
    };
    expect(round(omaha(8, true), table)?.table).toEqual(table);
  });
});

describe("payload", () => {
  // the sizes the format was chosen for: 10 header bits + 6 a slot
  it("packs hold'em on the river into 11 base64 chars", () => {
    const table: TableState = {
      hero: ids("Ah Ad"), villain: ids("Ks Kc"), board: ids("2c 7d 9s Th 4d"),
    };
    expect(encodeSpot(DEFAULTS, table)).toHaveLength(11);
  });

  it("packs the widest spot the felt can hold into 23", () => {
    expect(encodeSpot(omaha(8, true), {
      hero: ids("Ah As Kh Ks Qh Qs Jh Js"),
      villain: ids("Ad Ac Kd Kc Qd Qc Jd Jc"),
      board: ids("2c 7d 9s Th 4d"),
    })).toHaveLength(23);
  });

  it("stays in the base64url alphabet", () => {
    for (let n = 2; n <= 8; n++) {
      expect(encodeSpot(omaha(n), openingTable("omaha", n))).toMatch(/^[A-Za-z0-9_-]+$/);
    }
  });
});

describe("hold'em", () => {
  const holdem = (t: TableState) => round(DEFAULTS, t);

  it("keeps the default hole count, so omaha still opens on four", () => {
    // the encoded 2 describes hold'em's seats, not the omaha the visitor
    // might switch to next — that lands on four the way the picker does
    const got = holdem({ hero: ids("Ah Ad"), villain: [null, null], board: [] });
    expect(got?.settings).toEqual(DEFAULTS);
    expect(got?.settings.holeCards).toBe(4);
  });

  it("never comes back hi-lo", () => {
    const got = holdem({ hero: ids("Ah Ad"), villain: [null, null], board: [] });
    expect(got?.settings.himode).toBe("high");
  });
});

describe("rejects", () => {
  const table: TableState = {
    hero: ids("Ah Ad"), villain: ids("Ks Kc"), board: ids("2c 7d 9s"),
  };
  const good = encodeSpot(DEFAULTS, table);

  it("junk", () => {
    for (const s of ["", "!!!", "....", "a b", "%%%%%%"]) {
      expect(decodeSpot(s)).toBeNull();
    }
  });

  it("a payload too short for the shape its header claims", () => {
    expect(decodeSpot(good.slice(0, 6))).toBeNull();
  });

  it("a payload longer than the shape its header claims", () => {
    expect(decodeSpot(`${good}AAAA`)).toBeNull();
  });

  it("a card that appears twice", () => {
    const dupe: TableState = { ...table, villain: [cardId("Ah"), cardId("Kc")] };
    expect(decodeSpot(encodeSpot(DEFAULTS, dupe))).toBeNull();
  });

  it("a board slot left unknown", () => {
    // the felt cannot express it — the board is a prefix of dealt cards — so
    // this writes the unknown sentinel (52) where a board card belongs
    expect(decodeSpot(encodeSpot(DEFAULTS, { ...table, board: [52, cardId("7d")] })))
      .toBeNull();
  });

  it("a slot value that is not a card at all", () => {
    // 53..63 are unused: six bits hold more than a deck and a sentinel
    expect(decodeSpot(encodeSpot(DEFAULTS, { ...table, board: [61, cardId("7d")] })))
      .toBeNull();
  });

  it("every single-character truncation and extension", () => {
    for (let i = 0; i < good.length; i++) {
      expect(decodeSpot(good.slice(0, i))).toBeNull();
    }
  });
});

describe("the fragment", () => {
  const spot: SharedSpot = {
    settings: omaha(5, true), table: openingTable("omaha", 5),
  };
  const hash = spotHash(spot.settings, spot.table);

  it("carries the s= parameter and a leading #", () => {
    expect(hash.startsWith("#s=")).toBe(true);
  });

  it("reads back the spot it wrote", () => {
    expect(spotFromHash(hash)).toEqual(spot);
  });

  it("reads a fragment that carries no spot as none", () => {
    for (const h of ["", "#", "#other=1", "#s=", "#s=!!"]) {
      expect(spotFromHash(h)).toBeNull();
    }
  });
});
