// Background shader compilation: every kernel set the page can route to,
// starting with the game on screen. React-free; hooks/usePrewarm.ts mounts it.

import { prewarmEquity, type HiMode, type Variant } from "./equity.ts";

export interface PrewarmTarget { variant: Variant; himode: HiMode }

// which kernel set a target routes to; hold'em has no hi-lo, so both
// himodes name the same set there
const routeKey = (t: PrewarmTarget) =>
  (t.variant === "holdem" ? "holdem" : `omaha.${t.himode}`);

/** every kernel set the page can route to */
const TARGETS: readonly PrewarmTarget[] = [
  { variant: "holdem", himode: "high" },
  { variant: "omaha", himode: "high" },
  { variant: "omaha", himode: "hilo" },
];

/** TARGETS, with the set `current` routes to moved to the front */
export function prewarmOrder(current: PrewarmTarget): readonly PrewarmTarget[] {
  const k = routeKey(current);
  const first = TARGETS.find((t) => routeKey(t) === k) ?? TARGETS[0];
  return [first, ...TARGETS.filter((t) => t !== first)];
}

// requestIdleCallback is absent in Safari; the timeout stands in for it
const idle = () => new Promise<void>((resolve) => {
  if (window.requestIdleCallback) window.requestIdleCallback(() => resolve());
  else setTimeout(resolve, 500);
});

/**
 * Compile the current game's kernels immediately, then the rest one at a
 * time from an idle callback. `alive` is polled between sets: a compile in
 * flight can't be cancelled, but nothing more is queued behind a torn-down
 * page. Failures are swallowed; the solve path reports them if it matters.
 */
export async function prewarmAll(
  current: PrewarmTarget, alive: () => boolean = () => true,
): Promise<void> {
  const [first, ...rest] = prewarmOrder(current);
  await prewarmEquity(first).catch(() => {});
  if (!alive()) return;
  await idle();
  for (const t of rest) {
    if (!alive()) return;
    await prewarmEquity(t).catch(() => {});
  }
}
