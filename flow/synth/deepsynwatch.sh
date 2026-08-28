#!/usr/bin/env bash
# deepsynwatch.sh -- summarize live "&deepsyn -v" progress across restartloop lanes.
#
# Usage: bash flow/synth/deepsynwatch.sh [outdir]
#   outdir defaults to the most recently modified flow/build/restartloop_* dir.
#
# Reads each lane's abc.out (deepsyn -v prints one line per improvement, and
# restartloop.sh runs abc under stdbuf -oL, so the file is live mid-draw).
# One row per lane, best-first:
#   And      smallest And seen this draw
#   Lev      level on the most recent improvement line
#   iter     iteration count of the most recent improvement
#   draw     process time (min) of the most recent improvement
#   ago      minutes since abc.out was last written (= since last improvement)
#   imp/hr   improvements logged in the last hour of process time
# Note: this is the current DRAW only; completed-restart bests live in
# best.info (see restartstatus.sh).

set -u

OUT=${1:-}
if [ -z "$OUT" ]; then
    OUT=$(ls -1dt flow/build/restartloop_*/ 2>/dev/null | head -1)
    OUT=${OUT%/}
fi
if [ -z "$OUT" ] || [ ! -d "$OUT" ]; then
    echo "no restartloop output dir found (looked for flow/build/restartloop_*)" >&2
    exit 1
fi

echo "== deepsynwatch: $OUT  ($(date +%H:%M))  abc processes: $(pgrep -cx abc)"
printf '%-28s %6s %5s %6s %7s %6s %7s\n' lane And Lev iter draw ago imp/hr

now=$(date +%s)
for d in "$OUT"/*lane*/; do
    d=${d%/}
    lane=$(basename "$d")
    f="$d/abc.out"
    [ -s "$f" ] || continue
    mtime=$(stat -c %Y "$f")
    ago=$(( (now - mtime) / 60 ))
    awk -v lane="$lane" -v ago="$ago" '
        /Iter/ && /And =/ {
            iter=$2
            for (i = 1; i <= NF; i++) {
                if ($i == "Time") t = $(i+1)
                if ($i == "And")  a = $(i+2)
                if ($i == "Lev")  l = $(i+2)
            }
            if (best == "" || a+0 < best+0) best = a
            times[++n] = t
        }
        END {
            if (n == 0) exit
            recent = 0
            for (i = 1; i <= n; i++) if (times[i] >= times[n] - 3600) recent++
            printf "%-28s %6d %5d %6d %6.0fm %5dm %7d\n", \
                lane, best, l, iter, times[n]/60, ago, recent
        }' "$f"
done | sort -k2,2n
