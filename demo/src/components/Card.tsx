import type { KeyboardEvent, Ref } from "react";

import { rankChar, suitOf } from "../lib/cards.ts";

interface CardProps {
  /** null renders a face-down back (seat) or an empty dashed slot (board) */
  card: number | null;
  variant?: "back" | "slot";
  mini?: boolean;
  used?: boolean;
  /** accessible name */
  label?: string;
  title?: string;
  /** receives its own DOM node; the picker anchors against it */
  onActivate?: (el: HTMLElement) => void;
  className?: string;
  ref?: Ref<HTMLDivElement>;
}

/**
 * One card face. A div with button semantics rather than a <button>, to
 * keep UA button styling off a box that is sized four different ways.
 */
export function Card(
  { card, variant, mini, used, label, title, onActivate, className, ref }: CardProps,
) {
  const clickable = !!onActivate;
  const cls = [
    "card",
    card !== null ? suitOf(card).cls : variant === "slot" ? "slot" : "back",
    mini ? "mini" : "",
    used ? "used" : "",
    clickable ? "clickable" : "",
    className ?? "",
  ].filter(Boolean).join(" ");

  const onKeyDown = (ev: KeyboardEvent<HTMLDivElement>) => {
    if (ev.key === "Enter" || ev.key === " ") {
      ev.preventDefault();
      onActivate?.(ev.currentTarget);
    }
  };

  return (
    <div
      ref={ref}
      className={cls}
      title={title}
      onClick={onActivate && ((ev) => onActivate(ev.currentTarget))}
      onKeyDown={clickable ? onKeyDown : undefined}
      tabIndex={clickable ? 0 : undefined}
      role={clickable ? "button" : undefined}
      aria-label={label}
      aria-disabled={used ? true : undefined}
    >
      {card !== null && (
        <>
          <span className="rank">{rankChar(card)}</span>
          <span className="suit">{suitOf(card).ch}</span>
        </>
      )}
    </div>
  );
}
