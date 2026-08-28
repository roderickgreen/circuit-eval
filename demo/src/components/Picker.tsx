import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { cardName } from "../lib/cards.ts";
import { Card } from "./Card.tsx";

interface PickerProps {
  /** the slot that opened the picker; focus returns to it on close */
  anchor: HTMLElement;
  /** cards already committed elsewhere in the spot, shown but not pickable */
  used: Set<number>;
  onPick: (card: number) => void;
  onClose: () => void;
}

/**
 * 13x4 card-grid picker (PokerStove convention: ranks A..2 across, one row
 * per suit), anchored to the clicked slot. Cards already in use elsewhere are
 * disabled. Esc or an outside click closes without picking.
 *
 * Portalled to <body> so its fixed positioning can't be caught by a
 * transformed or clipping ancestor on the felt.
 */
export function Picker({ anchor, used, onPick, onClose }: PickerProps) {
  const el = useRef<HTMLDivElement>(null);
  const firstFree = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState<{ left: number; top: number } | null>(null);

  // focus returns to the anchor unless a re-render has replaced it
  const close = useCallback(() => {
    if (anchor.isConnected) anchor.focus({ preventScroll: true });
    onClose();
  }, [anchor, onClose]);

  // below the slot, clamped to the viewport; measured after layout so the
  // grid's real size is known
  useLayoutEffect(() => {
    const node = el.current;
    if (!node) return;
    const a = anchor.getBoundingClientRect();
    const p = node.getBoundingClientRect();
    setPos({
      left: Math.max(8, Math.min(a.left + a.width / 2 - p.width / 2,
        window.innerWidth - p.width - 8)),
      top: a.bottom + p.height + 8 <= window.innerHeight
        ? a.bottom + 6 : Math.max(8, a.top - p.height - 6),
    });
  }, [anchor]);

  useEffect(() => {
    firstFree.current?.focus({ preventScroll: true });
  }, []);

  useEffect(() => {
    const onDown = (ev: PointerEvent) => {
      if (!el.current?.contains(ev.target as Node)) close();
    };
    const onKey = (ev: KeyboardEvent) => { if (ev.key === "Escape") close(); };
    addEventListener("pointerdown", onDown, true);
    addEventListener("keydown", onKey, true);
    return () => {
      removeEventListener("pointerdown", onDown, true);
      removeEventListener("keydown", onKey, true);
    };
  }, [close]);

  let seenFree = false;
  const cells = [];
  for (let s = 0; s < 4; s++) {
    for (let r = 12; r >= 0; r--) {
      const id = 4 * r + s;
      const taken = used.has(id);
      const grabFocus = !taken && !seenFree;
      if (grabFocus) seenFree = true;
      cells.push(
        <Card
          key={id}
          ref={grabFocus ? firstFree : undefined}
          card={id}
          mini
          used={taken}
          title={taken ? `${cardName(id)} — already in the spot` : cardName(id)}
          label={cardName(id)}
          onActivate={taken ? undefined : () => { close(); onPick(id); }}
        />,
      );
    }
  }

  return createPortal(
    <div
      id="picker"
      ref={el}
      role="dialog"
      aria-label="choose a card"
      // hidden for the frame in which the position is measured
      style={pos ? { left: pos.left, top: pos.top } : { visibility: "hidden" }}
    >
      {cells}
    </div>,
    document.body,
  );
}
