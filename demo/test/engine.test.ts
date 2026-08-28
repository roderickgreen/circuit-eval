// The equity engine's seating policy, with no GPU and no renderer involved.
//
// Everything under test is arithmetic over a spot, and the store cases
// assert what the engine does when navigator.gpu is absent — which is
// exactly what a visitor on Firefox gets, and exactly what vitest's node
// environment provides natively. (Deno stopped being suitable here: it
// ships navigator.gpu unconditionally.)

import { describe, expect, it } from "vitest";

import { cardId, type Slot } from "../src/lib/cards.ts";
import { EquityEngine, planFor } from "../src/lib/engine.ts";
import { spotOf, type Spot } from "../src/lib/spot.ts";
import { openingTable } from "../src/lib/table.ts";

const ids = (s: string) => s.split(" ").map(cardId);
const unknown = (n: number) => Array<Slot>(n).fill(null);
const spot = (
  hero: Slot[], villain: Slot[], board: number[],
  variant: "holdem" | "omaha" = "holdem", himode: "high" | "hilo" = "high",
) => spotOf({ hero, villain, board }, variant, himode);

// what the panel would say, flattened to the two things a test cares about:
// which branch was taken, and whether the refusal could name a factorisation
const plan = (s: Spot) => {
  const p = planFor(s);
  return p.kind === "refused"
    ? `refused:${p.panel.kind === "refused" && p.panel.size === null ? "unbounded" : "sized"}`
    : p.kind;
};

// ---- policy ----
//
// The four branches, one representative spot each. The deal counts are the
// ones quoted in the source comments, so a change to either ceiling lands
// here rather than silently in front of a visitor.

describe("seating policy", () => {
  // every card face up: one deal, and it runs the moment it is seated
  it("solves a showdown on sight", () => {
    expect(plan(spot(ids("Ah Ad"), ids("Kh Kd"), ids("2c 7d 9s Th Jc"))))
      .toBe("auto");
  });

  // one unknown board card: 44 deals
  it("solves one unknown card on sight", () => {
    expect(plan(spot(ids("Ah Ad"), ids("Kh Kd"), ids("2c 7d 9s Th"))))
      .toBe("auto");
  });

  // hold'em flop against a known hand: 990 runouts
  it("solves a flop on sight", () => {
    expect(plan(spot(ids("Ah Ad"), ids("Kh Kd"), ids("2c 7d 9s"))))
      .toBe("auto");
  });

  // the opening spot, at both variants: ~10^9, past AUTO_MAX_DEALS, so it
  // states its size and waits for a click. This is the one every visitor sees.
  it("makes the hold'em opening wait for a click", () => {
    expect(plan(spotOf(openingTable("holdem", 2), "holdem", "high")))
      .toBe("pending");
  });
  it("makes the omaha opening wait for a click", () => {
    expect(plan(spotOf(openingTable("omaha", 4), "omaha", "high")))
      .toBe("pending");
  });

  // eight-card omaha against a wholly unknown hand: 6.7 x 10^13 deals is
  // inside the exact-tally bound, but the villain work list alone is
  // 177,232,627 entries — refused for the list, and able to name the
  // factorisation
  it("refuses a huge work list, with its size", () => {
    expect(plan(spot(ids("Ah As Kh Ks Qh Qs Jh Js"), unknown(8), [], "omaha")))
      .toBe("refused:sized");
  });

  // a cleared eight-card table: 5 x 10^22 deals, nothing to factorise
  it("refuses an uncountable spot, unsized", () => {
    expect(plan(spot(unknown(8), unknown(8), [], "omaha")))
      .toBe("refused:unbounded");
  });

  // The hi-lo quarter-pot factor is part of the exactness bound, not a
  // display detail: this spot's 3,006,687,713,309,280 deals sit under 2^53
  // but its quarters do not. High is refused too — the work list is far past
  // its own ceiling — but it gets there by the *second* check, so the panel
  // can still factorise it. Guards the order the two ceilings are tested in.
  it("counts hi-lo quarters toward the exact bound", () => {
    const hero = [...ids("Ah As Kh Ks Qh Qs Jh"), null];
    expect(plan(spot(hero, unknown(8), [], "omaha", "hilo")))
      .toBe("refused:unbounded");
    expect(plan(spot(hero, unknown(8), [], "omaha", "high")))
      .toBe("refused:sized");
  });
});

// ---- the store ----
//
// With no navigator.gpu every spot lands in the same place, which makes
// these cases about the engine's identity rules rather than about any solve.

describe("the store, with no GPU", () => {
  const table = { hero: ids("Ah Ad"), villain: ids("Kh Kd"), board: [] };
  const flop = { ...table, board: ids("2c 7d 9s") };

  const message = (engine: EquityEngine) => {
    const p = engine.getState().panel;
    return p.kind === "empty" ? p.message : p.kind === "refused" ? `refused:${p.why}` : p.kind;
  };

  it("guards the premise: this runtime has no navigator.gpu", () => {
    expect(navigator.gpu).toBeUndefined();
  });

  it("says nothing before a spot is seated", () => {
    expect(message(new EquityEngine())).toBe("no result");
  });

  it("explains itself when seated, and notifies once per change", () => {
    const engine = new EquityEngine();
    let notified = 0;
    engine.subscribe(() => { notified++; });

    engine.setSpot(spotOf(table, "holdem", "high"));
    expect(message(engine)).toBe("refused:gpu");
    expect(notified).toBe(1);

    // re-seating an identical spot notifies nobody...
    const state = engine.getState();
    engine.setSpot(spotOf(table, "holdem", "high"));
    engine.setSpot(spotOf(table, "holdem", "high"));
    expect(notified).toBe(1);
    // ...and hands back the same snapshot object: useSyncExternalStore
    // tears the tree down if the snapshot is rebuilt per read, so identity —
    // not just equality — is the contract here
    expect(engine.getState()).toBe(state);

    engine.setSpot(spotOf(flop, "holdem", "high"));
    expect(notified).toBe(2);
  });

  // clear drops the result and forgets the seated spot, so seating that same
  // spot again re-seats it — which is what lets the reset button re-run a
  // spot that resets back onto itself
  it("re-seats the same spot after a clear", () => {
    const engine = new EquityEngine();
    let notified = 0;
    engine.subscribe(() => { notified++; });

    engine.setSpot(spotOf(table, "holdem", "high"));
    engine.clear();
    expect(message(engine)).toBe("no result");
    expect(notified).toBe(2);

    engine.setSpot(spotOf(table, "holdem", "high"));
    expect(message(engine)).toBe("refused:gpu");
    expect(notified).toBe(3);
  });

  // dispose forgets the seated spot, so a re-mounted view re-seats from
  // scratch rather than finding the engine already holding this key
  it("re-seats after a dispose", () => {
    const engine = new EquityEngine();
    let notified = 0;
    engine.subscribe(() => { notified++; });

    engine.setSpot(spotOf(table, "holdem", "high"));
    engine.dispose();
    engine.setSpot(spotOf(table, "holdem", "high"));
    expect(notified).toBe(2);
  });

  it("stops notifying an unsubscribed listener", () => {
    const engine = new EquityEngine();
    let notified = 0;
    const stop = engine.subscribe(() => { notified++; });

    engine.setSpot(spotOf(table, "holdem", "high"));
    stop();
    engine.setSpot(spotOf(flop, "holdem", "high"));
    expect(notified).toBe(1);
  });
});
