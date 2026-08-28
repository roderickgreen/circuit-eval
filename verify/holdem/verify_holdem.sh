#!/bin/sh -e
# Exhaustive holdem verification of an arbitrary candidate netlist, at every
# arity the circuit claims to serve.
#
# Bitslices the candidate, compiles it, and links it into the C harnesses
# that carry PHE as an external oracle, then runs the two gates:
#
#   contract diff        dump_valmap enumerates all C(52,5) hands through the
#                        CANDIDATE, pairing its 24-bit value with PHE's rank
#                        per class, and the dump must be byte-identical to
#                        the table flow/encoding/mkspec.py generates (the
#                        encoding is defined in ENCODING.md). A candidate
#                        that changed the value encoding fails here and
#                        must not be shipped.
#   verify_nk            ordinal isomorphism of the raw 24-bit value vs PHE's
#                        native evaluate_5cards / 6cards / 7cards over the
#                        COMPLETE domain -- all C(52,5) + C(52,6) + C(52,7) =
#                        156,742,040 hands; also reports distinct-class
#                        counts (must be 7462 / 6075 / 4824) and split /
#                        merge / non-monotone counts.
#
# Together they pin the whole function exactly: the contract diff fixes the
# value of every 5-card class bit-for-bit, and the ordinal sweep proves the
# 6/7-card behavior collapses onto those same classes.  Exhaustive is cheap
# here: build ~0.5 s, verification ~24 s.
#
# The candidate's objects are built under their own prefix, NOT over
# verify/build/circuit_holdem.o -- that one belongs to the committed
# circuit and other tools link it.
#
# usage (from repo root): sh verify/holdem/verify_holdem.sh <candidate.blif> [workdir]

# NOT just the shebang: callers invoke this as `sh verify_holdem.sh ...`,
# which ignores the interpreter flags on line 1. Without this the script would
# plough past a failed build or a failed arity and exit 0 -- a gate that
# cannot fail.
set -e

BLIF=${1:?usage: verify_holdem.sh <candidate.blif> [workdir]}
WORK=${2:-artifacts/verify_holdem}
CMP=verify
B=$CMP/build

[ -f "$BLIF" ] || { echo "no such netlist: $BLIF" >&2; exit 1; }
mkdir -p "$WORK"

# oracle objects; shared, built once, reused
make -C $CMP build/phe.o build/phe6.o >/dev/null

# bs_* renamed at compile time (-D on the generated C), not with objcopy
# --redefine-sym: identical result, and a stock macOS toolchain has no
# objcopy.
python3 flow/codegen/cpu/bitslice.py "$BLIF" "$WORK/circuit.c"
${CC:-cc} -O2 -march=native -Iinclude \
    -Dbs_eval=bs_eval_holdem -Dbs_card_input=bs_card_input_holdem \
    -Dbs_num_outputs=bs_num_outputs_holdem -Dbs_num_inputs=bs_num_inputs_holdem \
    -Dbs_nb=bs_nb_holdem -c -o "$WORK/circuit.o" "$WORK/circuit.c"

PHEINC=third-party/PokerHandEvaluator/cpp/include
${CXX:-c++} -O3 -march=native -std=c++14 -w -I$PHEINC -o "$WORK/verify_nk" \
    $CMP/holdem/verify_nk.cpp "$WORK/circuit.o" $B/phe.o $B/phe6.o
${CXX:-c++} -O3 -march=native -std=c++14 -w -I$PHEINC -o "$WORK/dump_valmap" \
    $CMP/holdem/dump_valmap.cpp "$WORK/circuit.o" $B/phe.o

echo "== exhaustive holdem verification: $BLIF"
python3 flow/encoding/mkspec.py > "$WORK/valmap_spec.txt"
"$WORK/dump_valmap" > "$WORK/valmap_dump.txt"
diff "$WORK/valmap_spec.txt" "$WORK/valmap_dump.txt"
echo "encoding contract: 7462/7462 class values match mkspec.py"
for nh in 5 6 7; do
  "$WORK/verify_nk" $nh
done
