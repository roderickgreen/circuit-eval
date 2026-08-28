import type { HiMode, Variant } from "../lib/equity.ts";

interface ControlsProps {
  variant: Variant;
  holeCards: number;
  himode: HiMode;
  onVariant: (v: Variant) => void;
  onHoleCards: (n: number) => void;
  onHiMode: (m: HiMode) => void;
}

const HOLE_COUNTS = [2, 3, 4, 5, 6, 7, 8];

export function Controls(
  { variant, holeCards, himode, onVariant, onHoleCards, onHiMode }: ControlsProps,
) {
  const omaha = variant === "omaha";
  return (
    <section id="controls">
      <label>
        Variant
        <select value={variant} onChange={(e) => onVariant(e.target.value as Variant)}>
          <option value="holdem">Hold&apos;em</option>
          <option value="omaha">Omaha</option>
        </select>
      </label>
      <label id="nholeWrap" hidden={!omaha}>
        Hole cards
        <select value={holeCards} onChange={(e) => onHoleCards(+e.target.value)}>
          {HOLE_COUNTS.map((n) => <option key={n} value={n}>{n}</option>)}
        </select>
      </label>
      <label id="himodeWrap" hidden={!omaha}>
        <input
          type="checkbox"
          checked={himode === "hilo"}
          onChange={(e) => onHiMode(e.target.checked ? "hilo" : "high")}
        />
        {" "}Hi/Lo (8 or better)
      </label>
    </section>
  );
}
