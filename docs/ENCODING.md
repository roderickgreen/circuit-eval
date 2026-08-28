# The value encoding

The evaluators emit a 24-bit value:

```
bit 23      20 19      16 15      12 11       8 7        4 3        0
   +----------+----------+----------+----------+----------+----------+
   | category |  nibble4 |  nibble3 |  nibble2 |  nibble1 |  nibble0 |
   +----------+----------+----------+----------+----------+----------+
```

Kicker nibbles are rank values (0 = deuce .. 12 = ace), left-justified from
nibble4, unused nibbles zero:

| cat | category       | nibble4..0            |
|-----|----------------|-----------------------|
| 1   | high card      | a b c d e (descending)|
| 2   | one pair       | p k1 k2 k3 0          |
| 3   | two pair       | hi lo k 0 0           |
| 4   | trips          | t k1 k2 0 0           |
| 5   | straight       | h 0 0 0 0 (wheel h=3) |
| 6   | flush          | a b c d e             |
| 7   | full house     | t p 0 0 0             |
| 8   | quads          | q k 0 0 0             |
| 9   | straight flush | h 0 0 0 0             |

`value(x) > value(y)` iff hand x beats hand y; equality iff they tie. This
holds across 5-, 6-, and 7-card inputs (best-5-of-N).

## The low value

`circuit_eval_omaha_hilo` also emits an 8-bit low value: the rank mask of
the best 8-or-better low, bit 0 = ace .. bit 7 = eight, or `0xFF`
(`CIRCUIT_NO_LOW`) when no low qualifies. A low is five distinct ranks
all eight or lower, formed from exactly two hole cards and three board
cards; the ace counts as low and straights and flushes do not count
against it. Lows compare highest rank first, so with the eight in the top
bit the mask compares as an integer with SMALLER = stronger: the wheel
(A-2-3-4-5) is `0x1F`, the worst qualifying low (4-5-6-7-8) is `0xF8`.

## Card input convention

Input is a 52-bit presence vector, one bit per card, index `4*rank + suit`
(rank 0 = deuce .. 12 = ace). The evaluators are suit-symmetric, so the
suit labels only matter when card ids are exchanged with other code; the
README and `examples/` use suit 0 = spades, 1 = hearts, 2 = diamonds,
3 = clubs. The bitsliced kernels
expose `bs_card_input[card]` for the card -> input-plane position mapping.
