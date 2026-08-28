import { useEffect, useState } from "react";

import { prewarmAll, type PrewarmTarget } from "../lib/prewarm.ts";

/**
 * Starts the page's background shader compilation. Pipelines are cached in
 * equity/kernels.ts, so a second mount (StrictMode) compiles nothing.
 *
 * Only the mount value of `current` is read: it sets the order, and every
 * set gets compiled either way. A variant switched mid-run is reached by the
 * queue or by the engine's own prewarm await, into the same cache.
 */
export function usePrewarm(current: PrewarmTarget): void {
  const [atLoad] = useState(current);

  useEffect(() => {
    if (!navigator.gpu) return;
    let live = true;
    void prewarmAll(atLoad, () => live);
    return () => { live = false; };
  }, [atLoad]);
}
