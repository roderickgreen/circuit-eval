# netlists/ -- the verified netlists

The winning BLIFs from long synthesis runs; regenerating one costs the
original search time. Node counts are 2-input `.names` and appear in the
filenames. Every file passed the verification sequence before being added.
To promote a new netlist: add the file, keep the superseded one, update
the table.

| file | function | interface | verified |
|---|---|---|---|
| `holdem_value_1192.blif` | 7-card Hold'em -> 24-bit value, exact at 5/6/7 cards. lev 39 | `x{r}_{s}` / `e{b}` | exhaustive: all C(52,5)+C(52,6)+C(52,7) hands, ordinal + exact-through-the-map vs PHE |
| `omaha_high_2324.blif` | Omaha high; one netlist exact for PLO4/5/6. lev 106 | `h0..h51 b0..b51` / `ov0..ov23` | cec vs prepolish.aig; 120K bitsim x6 families and 1M PHE ordinal crosscheck at each of nhole 4/5/6 |
| `low_core_133.blif` | Omaha 8-or-better low core (16-input rank-presence form). lev 16 | `hp0..hp7 bp0..bp7` / `lo0..lo7` | via `low_composed_181` |
| `low_composed_181.blif` | `low_core_133` composed onto the 104 card planes. lev 17 | omaha planes / `lo0..lo7` | exhaustive: all 54,093 realizable rank-set pairs; plus 3x5,000 random deals |
| `cmp24_107.blif` | 24-bit unsigned comparator, `gt eq` per lane -- win/tie/loss over the 24-bit values, for fused equity kernels. lev 13 | `a0..a23 b0..b23` / `gt eq` | cec vs prepolish.aig and vs the spec BLIF (complete equivalence proof); 94,633-pair bitsim vs integer compare |
| `cmp8_35.blif` | 8-bit unsigned comparator, same contract, sized for the low-hand rank masks (smaller = stronger). lev 7 | `a0..a7 b0..b7` / `gt eq` | cec vs prepolish.aig and vs the spec BLIF; bitsim exhaustive over all 65,536 (a, b) pairs |

## Provenance and reproducibility

Every netlist came from a recorded `flow/synth/restartloop.sh` run:
read the normalized start, `&deepsyn -T 0 -J <J> -S <seed> [-t]`, then the
polish `&put; dc2; compress2rs; compress2rs; dc2; st; dch; if -K 2`.

The recipe identifies the run but does not reproduce it: deepsyn is not
bit-reproducible even at a fixed (start, seed, J) -- its choice pipeline
contains wall-clock budgets and SAT timeouts. The committed files are the
artifacts of record. Each synthesis winner ships with its raw
end-of-deepsyn AIG (`*.prepolish.aig`), cec-proven equivalent to its
.blif, so re-polish and chaining runs never repeat the original draw.

The comparators synthesize from the canonical-order normalized BLIF
(`make cmp8 cmp24`), not the raw AIG: yosys numbers AIG inputs by creation
order (b-bus-first here), so `flow/codegen/orderio.py` natural-sorts the
`.inputs` list and that BLIF is the order authority for synthesis,
renaming, and codegen.
