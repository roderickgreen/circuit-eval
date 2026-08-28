import { useCallback, useEffect, useState } from "react";

import { Controls } from "./components/Controls.tsx";
import { EquityPanel } from "./components/EquityPanel.tsx";
import { Felt } from "./components/Felt.tsx";
import { GpuBanner } from "./components/GpuBanner.tsx";
import { Picker } from "./components/Picker.tsx";
import { useEquityEngine } from "./hooks/useEquityEngine.ts";
import { usePrewarm } from "./hooks/usePrewarm.ts";
import { useSpotUrl } from "./hooks/useSpotUrl.ts";
import type { HiMode, Variant } from "./lib/equity.ts";
import { DEFAULTS, holeCountOf, saveSettings, type Settings }
  from "./lib/settings.ts";
import { openingState, type SharedSpot } from "./lib/share.ts";
import { spotOf } from "./lib/spot.ts";
import {
  clearBoard, clearSeat, clearSlot, dealStreet, openingTable, placeCard, randSeat,
  slotCard, tableUsed, type SeatZone, type TableState, type Zone,
} from "./lib/table.ts";

// which slot the open picker is aimed at; the pick applies to whatever the
// table holds when it lands, not a snapshot from when the slot was clicked
interface PickerTarget {
  zone: Zone;
  index: number;
  anchor: HTMLElement;
}

export function App() {
  // read once: the link's spot if there is one, else saved settings and that
  // game's opening hand
  const [opening] = useState(openingState);
  const [settings, setSettings] = useState<Settings>(opening.settings);
  const [table, setTable] = useState<TableState>(opening.table);
  const [picker, setPicker] = useState<PickerTarget | null>(null);

  useEffect(() => { saveSettings(settings); }, [settings]);

  // compile the kernels for the game we opened on first, then the others
  usePrewarm({ variant: settings.variant, himode: settings.himode });

  const spot = spotOf(table, settings.variant, settings.himode);
  const engine = useEquityEngine(spot);

  // ---- opening spot ----
  //
  // Changing variant or hole count, and the felt's reset button, re-seat
  // that game's opening spot; the hi-lo toggle re-scores in place. Seating
  // is idempotent, so reset on the opening hand has to clear the engine
  // first or the finished answer stays with no way to run it again.
  const clearResult = engine.clear;
  const seatOpening = useCallback((variant: Variant, nHole: number) => {
    setPicker(null);
    clearResult();
    setTable(openingTable(variant, nHole));
  }, [clearResult]);

  // a link pasted into a tab already on the page replaces the felt outright.
  // No clear needed: a link naming the seated spot raises no hashchange.
  const seatShared = useCallback((shared: SharedSpot) => {
    setPicker(null);
    setSettings(shared.settings);
    setTable(shared.table);
  }, []);

  useSpotUrl(settings, table, seatShared);

  const onVariant = useCallback((variant: Variant) => {
    // entering omaha starts from the defaults
    const next: Settings = variant === "omaha"
      ? { variant, holeCards: 4, himode: "high" }
      : { ...DEFAULTS, variant };
    setSettings(next);
    seatOpening(variant, holeCountOf(next));
  }, [seatOpening]);

  const onHoleCards = useCallback((holeCards: number) => {
    setSettings((s) => ({ ...s, holeCards }));
    seatOpening("omaha", holeCards);
  }, [seatOpening]);

  const onHiMode = useCallback((himode: HiMode) => {
    setSettings((s) => ({ ...s, himode }));
  }, []);

  const onReset = useCallback(() => {
    seatOpening(settings.variant, holeCountOf(settings));
  }, [seatOpening, settings]);

  // ---- table edits: lib/table.ts transitions bound to clicks ----

  // click a known card to forget it, an unknown slot to pick a card
  const onSlot = useCallback((zone: Zone, i: number, el: HTMLElement) => {
    if (slotCard(table, zone, i) !== null) {
      setPicker(null);
      setTable((t) => clearSlot(t, zone, i));
    } else {
      setPicker({ zone, index: i, anchor: el });
    }
  }, [table]);

  const onClearSeat = useCallback(
    (zone: SeatZone) => setTable((t) => clearSeat(t, zone)), []);
  const onRandSeat = useCallback(
    (zone: SeatZone) => setTable((t) => randSeat(t, zone)), []);
  const onDealStreet = useCallback(() => setTable(dealStreet), []);
  const onClearBoard = useCallback(() => setTable(clearBoard), []);

  return (
    <>
      <GpuBanner missing={!navigator.gpu} />
      <Controls
        variant={settings.variant}
        holeCards={settings.holeCards}
        himode={settings.himode}
        onVariant={onVariant}
        onHoleCards={onHoleCards}
        onHiMode={onHiMode}
      />
      <main>
        <Felt
          hero={table.hero}
          villain={table.villain}
          board={table.board}
          seat={engine.seat}
          onSlot={onSlot}
          onClearSeat={onClearSeat}
          onRandSeat={onRandSeat}
          onDealStreet={onDealStreet}
          onClearBoard={onClearBoard}
          onReset={onReset}
        />
        <EquityPanel view={engine} />
        {/* a grid child so the result can come before it on a phone */}
        <p id="hint">
          Every card is editable — click a dealt card to make it unknown, an
          empty or face-down one to choose it.
        </p>
      </main>
      {picker && (
        <Picker
          anchor={picker.anchor}
          // derived at render: the table can change while the picker is open
          // (the felt buttons stay reachable from the keyboard)
          used={tableUsed(table)}
          onPick={(c) => setTable((t) => placeCard(t, picker.zone, picker.index, c))}
          onClose={() => setPicker(null)}
        />
      )}
    </>
  );
}
