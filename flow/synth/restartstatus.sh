#!/usr/bin/env bash
# Progress summary for restartloop.sh: per top and arm, restarts done,
# restart duration spread, best nd vs baseline, per-lane tail, and the
# paired plain-vs-t seed table.  Ends with a one-line-per-top SUMMARY
# block so a growing report stays scannable from the bottom.
# usage: restartstatus.sh [outdir]   (default: most recently launched run)
set -u
cd "$(dirname "$0")/../.."
OUT=${1:-$(ls -td flow/build/restartloop_j*/ 2>/dev/null | head -1)}
OUT=${OUT%/}
[ -n "$OUT" ] || { echo "no restartloop output dirs found"; exit 0; }

# per-top baseline nd (the spec-start netlists these runs try to beat)
baseline() {
    case "$1" in
        holdem_value_io)  echo 1206 ;;
        omaha_value_io) echo 2346 ;;
        low_core_io)    echo 133 ;;
        *)              echo "?" ;;
    esac
}

echo "abc processes: $(pgrep -cx abc 2>/dev/null; true)   ($OUT)"
tops=$(ls -d "$OUT"/*_lane* 2>/dev/null | sed 's/.*\///; s/_lane[0-9]*t\{0,1\}$//' | sort -u)
summary=""
for top in $tops; do
    c=$(baseline "$top")
    n_plain=- b_plain=- n_t=- b_t=-
    for arm in plain t; do
        if [ "$arm" = plain ]; then
            dirs=$(ls -d "$OUT/${top}"_lane*[0-9] 2>/dev/null)
            label="$top plain"
        else
            dirs=$(ls -d "$OUT/${top}"_lane*t 2>/dev/null)
            label="$top -t"
        fi
        [ -n "$dirs" ] || continue
        logs=$(for d in $dirs; do [ -s "$d/log" ] && echo "$d/log"; done)
        done_n=$(cat $logs 2>/dev/null | grep -c "nd [0-9]* (best")
        best=$(sed -n 's/.*nd=\([0-9]*\).*/\1/p' $(for d in $dirs; do echo "$d/best.info"; done) 2>/dev/null | sort -n | head -1)
        eval "n_$arm=$done_n; b_$arm=${best:-none}"
        echo
        echo "== $label: $done_n restarts done, best ${best:-none} (baseline $c)"
        grep -h "nd [0-9]* (best" $logs /dev/null 2>/dev/null \
            | sed 's/.* seed [0-9]*: \([0-9]*m\)[0-9]*s.*/\1/' | sort -n | uniq -c \
            | sort -k2 -n | awk '{printf "  %s x%s", $2, $1}'
        echo "   (restart durations, minute buckets)"
        for d in $dirs; do
            last=$(tail -1 "$d/log" 2>/dev/null)
            age=""
            ts=$(date -d "$(echo "${last%% *}" | tr / ' ')" +%s 2>/dev/null) || ts=""
            [ -n "$ts" ] && age="   [current draw $(( ($(date +%s) - ts) / 60 ))m in]"
            echo "  $(basename "$d"): $last$age"
        done
    done
    # in-flight draw ages: time since each lane's last log line (a draw
    # starts the moment the previous one is logged)
    inflight=$(for d in "$OUT/${top}"_lane*; do
        ts=$(tail -1 "$d/log" 2>/dev/null | awk '{print $1}' | tr / ' ')
        [ -n "$ts" ] && date -d "$ts" +%s 2>/dev/null
    done | sort -n | awk -v now="$(date +%s)" \
        'NR==1{old=$1} {new=$1} END{if(NR) printf "in-flight %dm-%dm", (now-new)/60, (now-old)/60}')
    # paired per-seed table: seeds finished by BOTH arms (only when both
    # arms exist, i.e. an ARMS=both run)
    pt=""
    if [ "$n_plain" != - ] && [ "$n_t" != - ]; then
    echo "  paired plain-vs-t (negative delta = -t won):"
    pt=$(join <(grep -h "nd [0-9]* (best" "$OUT/${top}"_lane*[0-9]/log 2>/dev/null \
             | sed 's/.*seed \([0-9]*\): .* nd \([0-9]*\) .*/\1 \2/' | sort -n) \
         <(grep -h "nd [0-9]* (best" "$OUT/${top}"_lane*t/log 2>/dev/null \
             | sed 's/.*seed \([0-9]*\): .* nd \([0-9]*\) .*/\1 \2/' | sort -n) 2>/dev/null \
        | awk '{d=$3-$2; s=(d<0?"":"+"); print "    seed "$1": plain "$2"  -t "$3"  delta "s d
               a[n++]=d; t+=d; if(d<0) tw++; else if(d>0) pw++; else tie++}
               END{if(!n) exit
                   asort(a); med=(n%2 ? a[(n+1)/2] : (a[n/2]+a[n/2+1])/2)
                   printf "    (%d pairs: -t wins %d, plain wins %d, ties %d; mean %+.1f, median %+.1f)\n",
                          n, tw+0, pw+0, tie+0, t/n, med}')
    [ -n "$pt" ] && echo "$pt"
    fi
    pline=$(echo "$pt" | tail -1 | sed 's/^ *//; s/^(\(.*\))$/\1/')
    if [ "$n_plain" = - ] || [ "$n_t" = - ]; then pline="single-arm"; fi
    summary+=$(printf '  %-15s base %-5s | plain %s done best %s | -t %s done best %s | %s | %s' \
               "$top" "$c" "$n_plain" "$b_plain" "$n_t" "$b_t" "${inflight:-nothing in flight}" \
               "${pline:-no pairs yet}")$'\n'
done
# elapsed since launch: RUNNING is stamped at launch (falls back to the
# earliest lane-log line; a relaunch restamps, so this is the current run)
t0=$( { head -1 "$OUT/RUNNING" 2>/dev/null; head -qn1 "$OUT"/*_lane*/log 2>/dev/null; } \
     | awk '{print $1}' | sort | head -1)
elapsed=""
if [ -n "$t0" ]; then
    es=$(( $(date +%s) - $(date -d "${t0/\// }" +%s) ))
    elapsed=", elapsed $((es / 3600))h$(printf %02d $((es % 3600 / 60)))m"
fi
echo
echo "== SUMMARY $(date +%H:%M)$elapsed"
printf '%s' "$summary"
exit 0
