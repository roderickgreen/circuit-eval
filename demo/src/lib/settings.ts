import type { HiMode, Variant } from "./equity.ts";

// Persisted variant + settings. Switching to omaha resets to defaults.
const SETTINGS_KEY = "pokerEvalSettings.v1";

export interface Settings {
  variant: Variant;
  holeCards: number;
  himode: HiMode;
}

export const DEFAULTS: Settings = { variant: "holdem", holeCards: 4, himode: "high" };

export function loadSettings(): Settings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    const s = raw ? JSON.parse(raw) : null;
    if (s?.variant === "holdem") return { ...DEFAULTS, variant: "holdem" };
    if (s?.variant === "omaha") {
      const n = Number(s.holeCards);
      return {
        variant: "omaha",
        holeCards: Number.isInteger(n) && n >= 2 && n <= 8 ? n : 4,
        himode: s.highLow ? "hilo" : "high",
      };
    }
  } catch { /* corrupt or unavailable storage: no settings */ }
  return DEFAULTS;
}

export function saveSettings(s: Settings) {
  // hold'em carries neither field, so a stale hole count can't leak back in
  const stored = s.variant === "omaha"
    ? { variant: "omaha", holeCards: s.holeCards, highLow: s.himode === "hilo" }
    : { variant: "holdem" };
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(stored));
  } catch { /* private mode, quota */ }
}

/** hold'em is always two hole cards; the picker only applies to omaha */
export const holeCountOf = (s: Settings) => (s.variant === "holdem" ? 2 : s.holeCards);
