import { cardName, nextStreetCount, STREET_NAME, type Slot } from "../lib/cards.ts";
import type { SeatDisplay } from "../lib/engine.ts";
import type { SeatZone, Zone } from "../lib/table.ts";
import { Card } from "./Card.tsx";

interface FeltProps {
  hero: Slot[];
  villain: Slot[];
  board: number[];
  seat: SeatDisplay | null;
  onSlot: (zone: Zone, index: number, el: HTMLElement) => void;
  onClearSeat: (zone: SeatZone) => void;
  onRandSeat: (zone: SeatZone) => void;
  onDealStreet: () => void;
  onClearBoard: () => void;
  onReset: () => void;
}

/** the seat's equity readout, or its verdict once the hand is decided */
function SeatEq({ seat, side }: { seat: SeatDisplay | null; side: SeatZone }) {
  if (!seat) return <div className="eq" />;
  if (seat.kind === "verdict") {
    const c = seat[side];
    return <div className={`eq verdict ${c.cls}`}>{c.text}</div>;
  }
  return (
    <div className="eq">
      {seat[side]}
      {/* the seat number is equity (wins plus half the ties), not the panel's
          "Hero wins" figure; labelled once, on the hero side */}
      {side === "hero" && seat.hero ? <span className="cap">equity</span> : null}
    </div>
  );
}

function SeatCards(
  { cards, zone, onSlot }:
  { cards: Slot[]; zone: SeatZone; onSlot: FeltProps["onSlot"] },
) {
  return (
    <div className="cards">
      {cards.map((c, i) => {
        const title = c === null ? "unknown card — pick one" : "make unknown (any card)";
        return (
          <Card
            key={i}
            card={c}
            title={title}
            label={c === null ? title : `${cardName(c)} — ${title}`}
            onActivate={(el) => onSlot(zone, i, el)}
          />
        );
      })}
    </div>
  );
}

function SeatName(
  { who, zone, onClear, onRand }:
  { who: string; zone: SeatZone; onClear: () => void; onRand: () => void },
) {
  return (
    <div className="who">
      {who}
      <button
        className="felt-btn"
        onClick={onClear}
        aria-label={`clear the ${zone === "hero" ? "hero's" : "villain's"} hand`}
        title="make the whole hand unknown — equity vs any hand"
      >clear</button>
      <button
        className="felt-btn"
        onClick={onRand}
        aria-label={`deal the ${zone} a random hand`}
        title="deal this seat a random hand"
      >rand</button>
    </div>
  );
}

export function Felt({
  hero, villain, board, seat,
  onSlot, onClearSeat, onRandSeat, onDealStreet, onClearBoard, onReset,
}: FeltProps) {
  // the label stays "deal" so the button's width never changes; the tooltip
  // and accessible name carry the street
  const next = nextStreetCount(board.length);
  const streetTitle = next === null
    ? "the board is complete — click a card to take it back"
    : `deal the ${STREET_NAME[next]}`;

  return (
    <section id="tableCol">
      <div id="table">
        <button
          id="resetBtn"
          onClick={onReset}
          title="reset to the opening hand"
          aria-label="reset to the opening hand"
        >
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2"
               strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8" />
            <path d="M21 3v5h-5" />
          </svg>
        </button>

        <div className="seat" id="villainSeat">
          <SeatName who="Villain" zone="villain"
                    onClear={() => onClearSeat("villain")}
                    onRand={() => onRandSeat("villain")} />
          <SeatCards cards={villain} zone="villain" onSlot={onSlot} />
          <SeatEq seat={seat} side="villain" />
        </div>

        <div id="community">
          <button id="clearBoardBtn" className="felt-btn" onClick={onClearBoard}
                  disabled={board.length === 0}
                  title="take the whole board back" aria-label="clear the board">
            clear
          </button>
          <div className="cards">
            {Array.from({ length: 5 }, (_, i) => {
              const c = i < board.length ? board[i] : null;
              const title = c === null ? "empty board slot — pick a card" : "un-deal";
              return (
                <Card
                  key={i}
                  card={c}
                  variant="slot"
                  title={title}
                  label={c === null ? title : `${cardName(c)} — ${title}`}
                  onActivate={(el) => onSlot("board", i, el)}
                />
              );
            })}
          </div>
          <button id="streetBtn" className="felt-btn" onClick={onDealStreet}
                  disabled={next === null} title={streetTitle} aria-label={streetTitle}>
            deal
          </button>
        </div>

        <div className="seat" id="heroSeat">
          <SeatEq seat={seat} side="hero" />
          <SeatCards cards={hero} zone="hero" onSlot={onSlot} />
          <SeatName who="Hero" zone="hero"
                    onClear={() => onClearSeat("hero")}
                    onRand={() => onRandSeat("hero")} />
        </div>
      </div>
    </section>
  );
}
