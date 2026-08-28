#!/usr/bin/env bash
# Endless deterministic random-restart deepsyn -- runs until killed.
#
# Every restart is an independent lottery from the RAW start with a fresh
# seed, run with `&deepsyn -T 0 -J <J>` so it self-terminates once it
# stalls (J consecutive steps without improvement) instead of holding a
# core on a dead trajectory; the next seed then begins.
#
# J is the stall tolerance, one FIXED value for every top.  Small J
# maximizes restart counts; a long run at J=500-1000 lets slow-stepping
# tops ride out long trajectories -- large circuits reward exactly that
# (their best draws are the longest ones), while fast-stepping tops pack
# in restarts either way.
#
# SEED0 offsets every lane's seed walk (lane k runs SEED0+k, SEED0+k+PAIRS,
# ...).  A seed already explored at a HIGHER J is a dominated prefix at a
# lower one, so new experiments should start from fresh seed territory.
#
# ARMS picks which arms run: "both", "t" or "plain" (PAIRS lanes per top,
# single arm).  Under "both", each top runs PAIRS plain lanes and PAIRS -t
# lanes, and arm pairs SHARE their seed walk (lane k and lane kt both run
# seeds SEED0+k, SEED0+k+PAIRS, ...), so plain-vs-t is a paired per-seed
# comparison -- the only kind the draw-to-draw spread allows.  -t aligns
# deepsyn's cost with the mapped node count scored here, so single-arm
# runs should use ARMS=t.
#
# TOPS picks which tops run: "all" (default) or a space-separated subset
# of the job names (holdem_value_io, omaha_value_io), e.g.
# TOPS=omaha_value_io PAIRS=24 SEED0=3000 for an omaha-only fleet.  OUT
# overrides the output dir (default flow/build/restartloop_j$J);
# give a new experiment its own dir, or lanes resume against the old
# run's best.info thresholds and pollute its stats.
#
# abc runs with deepsyn -v (improvement lines: "Iter N : Time T sec :
# And = A") and stdout line-buffered (stdbuf), so $lane/abc.out shows
# progress DURING a draw, not only after it exits.
#
# CAVEAT: deepsyn is NOT bit-reproducible
# run-to-run even at fixed (start, seed, J) -- abc's dch/dar choice
# pipeline contains wall-clock budgets and SAT timeouts, so sub-command
# outcomes depend on machine load.  The seed pins the recipe lottery,
# not the result; treat (seed, J) in best.info as identification, not a
# rebuild recipe, and keep best.aig/best.blif as the artifacts of record.
#
# Every new per-lane best saves BOTH the polished mapped netlist
# (best.blif, what is scored) and the raw end-of-deepsyn AIG (best.aig) --
# re-polish and chaining runs want the unpolished artifact too, without
# paying the draw's runtime again to regenerate it.
#
# Beyond the best ratchet, EVERY completed draw is kept in the lane
# dir as s<seed>_and<A>_nd<N>.{aig,blif}: raw and-count and
# mapped nd can fork (the smallest-and endpoint is not the smallest-nd
# one), draws are unreproducible, and the files are tiny (~130 KB/draw),
# so discarding non-record draws throws away candidates.  <A> is the
# smallest And from deepsyn's -v improvement lines ("x" if the draw
# logged none, i.e. it never beat its start).
#
# status:  bash flow/synth/restartstatus.sh [outdir]
# stop:    pkill -f restartloop.sh; pkill -x abc
# usage:   [J=100] [PAIRS=4] [SEED0=1000] [ARMS=both] [TOPS=all] [OUT=...] [JOBS="name:start ..."] bash flow/synth/restartloop.sh
set -u
cd "$(dirname "$0")/../.."
ABC=third-party/abc/abc
RC="source third-party/abc/abc.rc"
J=${J:-100}
PAIRS=${PAIRS:-4}
SEED0=${SEED0:-1000}
ARMS=${ARMS:-both}
TOPS=${TOPS:-all}
OUT=${OUT:-flow/build/restartloop_j${J}}
mkdir -p "$OUT"

# JOBS is overridable to restart-sweep any start artifact, e.g. chaining
# from a previous best's end-of-deepsyn state:
#   JOBS="omaha_2324pp:flow/netlists/omaha_high_2324.prepolish.aig"
JOBS=${JOBS:-"holdem_value_io:flow/build/holdem_value_io.aig
omaha_value_io:flow/build/omaha_value_io.aig"}

run_lane() {
    local top=$1 start=$2 tflag=$3 lane=$4
    local dir="$OUT/${top}_lane${lane}${tflag:+t}"
    mkdir -p "$dir"
    case "$start" in
        *.aig) local READ="&r $start" ;;
        *)     local READ="read_blif $start; st; &get" ;;
    esac
    local seed=$((SEED0 + lane)) best=9999999 r=0 n t0 dt
    [ -s "$dir/best.info" ] && best=$(sed -n 's/.*nd=\([0-9]*\).*/\1/p' "$dir/best.info")
    echo "$(date +%F/%H:%M:%S) $top lane $lane${tflag:+t} start (J $J${tflag:+ $tflag}, first seed $seed, best $best)" >> "$dir/log"
    while :; do
        r=$((r + 1))
        t0=$(date +%s)
        stdbuf -oL $ABC -c "$RC; $READ; &deepsyn -T 0 -J $J -S $seed -v $tflag; \
&w $dir/round.aig; \
&put; dc2; compress2rs; compress2rs; dc2; st; dch; if -K 2; \
write_blif $dir/round.blif" > "$dir/abc.out" 2>&1
        dt=$(( $(date +%s) - t0 ))
        if [ ! -s "$dir/round.blif" ]; then
            echo "$(date +%F/%H:%M:%S) $top lane $lane${tflag:+t} restart $r seed $seed: abc FAILED after ${dt}s" >> "$dir/log"
            sleep 10
        else
            n=$(grep -c "^\.names" "$dir/round.blif")
            if [ "$n" -lt "$best" ]; then
                cp "$dir/round.blif" "$dir/best.blif"
                cp "$dir/round.aig" "$dir/best.aig" 2>/dev/null
                echo "top=$top seed=$seed J=$J tflag=$tflag nd=$n date=$(date +%F/%H:%M:%S)" > "$dir/best.info"
                best=$n
            fi
            a=$(awk '/Iter/ && /And =/ {
                    for (i = 1; i <= NF; i++) if ($i == "And") v = $(i+2)
                    if (m == "" || v+0 < m+0) m = v
                } END { print m }' "$dir/abc.out")
            mv "$dir/round.aig" "$dir/s${seed}_and${a:-x}_nd${n}.aig" 2>/dev/null
            mv "$dir/round.blif" "$dir/s${seed}_and${a:-x}_nd${n}.blif"
            echo "$(date +%F/%H:%M:%S) $top lane $lane${tflag:+t} restart $r seed $seed: $((dt / 60))m$((dt % 60))s, nd $n (best $best)" >> "$dir/log"
        fi
        rm -f "$dir/round.blif" "$dir/round.aig"
        seed=$((seed + PAIRS))    # advance even on failure: never re-run a seed
    done
}

# Reproducibility stamp: deterministic results are only as reproducible
# as the toolchain that made them.  yosys and abc are both pinned by
# submodules, and the start .aig hashes pin the exact yosys output the
# seeds ran from.
YOSYS=${YOSYS:-third-party/yosys/build/yosys}
{
    echo "yosys: $($YOSYS --version 2>/dev/null || echo 'not found')"
    echo "yosys submodule: $(git -C third-party/yosys rev-parse HEAD 2>/dev/null)"
    echo "abc submodule: $(git -C third-party/abc rev-parse HEAD 2>/dev/null)"
    echo "abc binary md5: $(md5sum $ABC | cut -d' ' -f1)"
    for spec in $JOBS; do
        f=${spec#*:}
        [ -s "$f" ] && md5sum "$f" | sed 's/^/start /'
    done
} > "$OUT/VERSIONS"

pids=()
trap 'kill "${pids[@]}" 2>/dev/null; exit' INT TERM
for spec in $JOBS; do
    IFS=: read -r top start <<< "$spec"
    if [ "$TOPS" != all ]; then
        case " $TOPS " in
            *" $top "*) ;;
            *) continue ;;
        esac
    fi
    if [ ! -s "$start" ]; then
        echo "SKIP $top: missing $start (run make / synth.sh first)"
        continue
    fi
    for lane in $(seq 0 $((PAIRS - 1))); do
        case "$ARMS" in
            both)  run_lane "$top" "$start" ""   "$lane" & pids+=($!)
                   run_lane "$top" "$start" "-t" "$lane" & pids+=($!) ;;
            t)     run_lane "$top" "$start" "-t" "$lane" & pids+=($!) ;;
            plain) run_lane "$top" "$start" ""   "$lane" & pids+=($!) ;;
        esac
    done
done
echo "$(date +%F/%H:%M:%S) ${#pids[@]} lanes running until killed (J=$J; pkill -f restartloop.sh; pkill -x abc)" | tee "$OUT/RUNNING"
wait
