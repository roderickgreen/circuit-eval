#!/bin/sh
# Code/data footprint of every evaluator in the benchmark tables -- the
# non-timed "benchmark".  code = executable sections, data = table
# sections (.text* vs .rodata*/.data*/.bss*).  Section-level, not
# Berkeley `size`, which folds .rodata into its text column and would
# credit PHE's tables as code.  TwoPlusTwo's table is a file loaded at
# runtime, not a section, so it is counted from disk; its evaluation
# code is the 7-lookup chase inlined into the harness.
#
# Linux only: Apple's cctools size has no -A, and parsing its
# -m output didn't survive contact with a real mac.  The data column
# is identical everywhere and the code column's story (KB vs MB)
# doesn't change with the ISA, so the Linux table stands in.
#
# usage: sizes.sh [builddir]   (run from bench/)
B=${1:-build}
# the two products that live outside the build dir: TwoPlusTwo's runtime
# table, and the arch-independent PHE plo table objects the Makefile
# compiles once and shares across builds (see its clean targets)
HANDRANKS=../artifacts/HandRanks.dat
TBL=../artifacts/phe-tables


if [ "$(uname)" = Darwin ]; then
    echo "sizes.sh: skipped on macOS (Linux-only; footprint is the same table)"
    exit 0
fi

sum() {
    size -A -d "$@" | awk '
        $1 ~ /^\.text/                                    { code += $2 }
        $1 ~ /^\.(rodata|data|bss)/                       { data += $2 }
        END { printf "%d %d", code, data }'
}

row() {
    name=$1; note=$2; shift 2
    set -- $(sum "$@")
    printf "  %-20s %10s %12s   %s\n" "$name" "$1" "$2" "$note"
}

echo
echo "evaluator footprint (bytes; code = text sections, data = rodata+data+bss)"
printf "  %-20s %10s %12s\n" "" code data
row CircuitEvalHoldem "holdem circuit + conversion layer (circuit_eval + bspack)" \
    $B/circuit_holdem.o $B/circuit_eval.o $B/bspack.o
if [ -f $HANDRANKS ]; then
    printf "  %-20s %10s %12s   %s\n" TwoPlusTwo "~0" \
        "$(wc -c < $HANDRANKS | tr -d ' 	')" \
        "chase is 7 inline loads; data = HandRanks.dat"
else
    printf "  %-20s %10s %12s   %s\n" TwoPlusTwo "~0" "?" \
        "HandRanks.dat not built"
fi
row OMPEval "" $B/omp_eval.o
row PHE     "7-card evaluator" $B/phe7.o
row CircuitEvalOmaha "omaha circuit + conversion layer" \
    $B/circuit_omaha.o $B/circuit_eval.o $B/bspack.o
row CircuitEvalOmahaHiLo "+ low circuit" \
    $B/circuit_omaha.o $B/circuit_omaha_low.o $B/circuit_eval.o $B/bspack.o
row PHEOmaha4 "" $B/phe_plo_4.o $TBL/tables_plo4.o
row PHEOmaha5 "" $B/phe_plo_5.o $TBL/tables_plo5.o
row PHEOmaha6 "" $B/phe_plo_6.o $TBL/tables_plo6.o
echo "  (PHE omaha rows also call ~1 KB of helpers counted under PHE)"
