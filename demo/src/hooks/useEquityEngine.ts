import { useEffect, useMemo, useState, useSyncExternalStore } from "react";

import { EquityEngine, type EngineState } from "../lib/engine.ts";
import type { Spot } from "../lib/spot.ts";

/** what a view reads: the engine's display state plus its five controls */
export interface EquityView extends EngineState {
  compute: () => void;
  pause: () => void;
  resume: () => void;
  stop: () => void;
  /** forget the result; the seating effect re-seats on the next commit */
  clear: () => void;
}

/**
 * Binds one EquityEngine to the tree and keeps it seated on `spot`. The
 * seating effect has no dependency array: setSpot is idempotent on the
 * spot's key, so running it after every commit is the rule stated once.
 */
export function useEquityEngine(spot: Spot): EquityView {
  const [engine] = useState(() => new EquityEngine());
  const state = useSyncExternalStore(engine.subscribe, engine.getState, engine.getState);

  useEffect(() => { engine.setSpot(spot); });
  useEffect(() => () => engine.dispose(), [engine]);

  // the controls are stable, so this changes only when the display does
  return useMemo(() => ({
    ...state,
    compute: engine.compute,
    pause: engine.pause,
    resume: engine.resume,
    stop: engine.stop,
    clear: engine.clear,
  }), [state, engine]);
}
