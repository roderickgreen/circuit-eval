import { useEffect } from "react";

import { spotFromHash, spotHash, type SharedSpot } from "../lib/share.ts";
import type { Settings } from "../lib/settings.ts";
import type { TableState } from "../lib/table.ts";

/**
 * Keeps the address bar and the felt naming the same spot, both ways.
 *
 * Outward: every edit rewrites the fragment via `replaceState`, so a card
 * click adds no history entry. Inward: a link pasted into a tab already on
 * the page changes only the fragment and does not reload, so `hashchange`
 * hands the spot to `onLink`. The two cannot chase each other because
 * `replaceState` fires no `hashchange`. The page's first read is a state
 * initializer instead (`openingState` in App.tsx).
 */
export function useSpotUrl(
  settings: Settings,
  table: TableState,
  onLink: (spot: SharedSpot) => void,
): void {
  // the string is the effect's dependency, so a new table object holding
  // the same cards rewrites nothing
  const hash = spotHash(settings, table);

  useEffect(() => {
    history.replaceState(null, "", hash);
  }, [hash]);

  useEffect(() => {
    const onHash = () => {
      const shared = spotFromHash(location.hash);
      if (shared) onLink(shared);
    };
    addEventListener("hashchange", onHash);
    return () => removeEventListener("hashchange", onHash);
  }, [onLink]);
}
