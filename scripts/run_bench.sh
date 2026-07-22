#!/usr/bin/env bash
# Full measurement suite. Produces docs/RESULTS.md plus the raw JSON/CSV behind
# every figure in it, so the numbers in the docs can be regenerated and diffed
# rather than trusted.
#
# Usage: scripts/run_bench.sh [quick|full]
set -uo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-full}
BIN=${BIN:-build}
OUT=${OUT:-docs/results}
mkdir -p "$OUT"

if [ ! -x "$BIN/ge_loadgen" ]; then
  echo "no build found at $BIN -- run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

if [ "$MODE" = quick ]; then DUR=4; WARM=2; else DUR=15; WARM=4; fi
strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }

# Sized to the *headline* measurement (step 3), which runs 2 I/O threads,
# 2 shards and 2 generator threads and consumes well under a core of CPU -- it
# needs enough free cores that none of its threads waits on a runqueue, not
# enough to saturate them. The sweep's top points and the scaling table want
# considerably more headroom; those carry their own caveats in the report, and
# the highest sweep points are flagged generator-CPU-bound on any laptop.
NEEDED_CORES=4

read -r FREE_CORES TOTAL_CORES IDLE_PCT LOAD1 <<<"$(./scripts/machine_state.sh)"
printf '%s free cores of %s (%s%% idle, load average %s)\n' \
    "$FREE_CORES" "$TOTAL_CORES" "$IDLE_PCT" "$LOAD1" | tee "$OUT/machine-state-before.txt"

# Refuse to present tail latencies from a contended machine as if they
# characterised the server. p50 and the microbenchmarks survive contention;
# p99 and beyond do not, because the measurement brackets get preempted.
if awk -v f="$FREE_CORES" -v n="$NEEDED_CORES" 'BEGIN{exit !(f < n)}'; then
  cat <<WARN

  ############################################################################
  #  ONLY $FREE_CORES OF $TOTAL_CORES CORES ARE FREE; THIS SUITE WANTS $NEEDED_CORES.
  #
  #  p50 and the component microbenchmarks will still be meaningful. Every tail
  #  figure (p99 and beyond) will be dominated by scheduler preemption rather
  #  than by the server, and the thread-scaling table will be meaningless
  #  because there are no free cores to scale onto.
  #
  #  The run will proceed and RESULTS.md will carry this warning, but do not
  #  quote the tails from it. Re-run on an idle machine for numbers worth
  #  publishing.
  ############################################################################

WARN
fi

echo
echo "=== 1/6  unit tests ==="
"$BIN/ge_tests" | strip_ansi | tee "$OUT/tests.txt" | tail -3

echo
echo "=== 2/6  component microbenchmarks ==="
"$BIN/ge_bench" | strip_ansi > "$OUT/microbench.txt"
tail -n +4 "$OUT/microbench.txt" | head -60

echo
echo "=== 3/6  steady-state latency, 100 clients ==="
"$BIN/ge_loadgen" --clients 100 --client-threads 2 --io-threads 2 --shards 2 \
    --ping-rate 50 --move-rate 30 --duration "$DUR" --warmup "$WARM" --spin-us 500 \
    --json "$OUT/latency-100.json" --hist-csv "$OUT/dist" | strip_ansi > "$OUT/latency-100.txt"
grep -E "ping RTT \(wire|server ping service|input -> state" -A2 "$OUT/latency-100.txt"

echo
echo "=== 4/6  spin-versus-block trade at fixed load ==="
{
  echo "spin_us,p50_ns,p99_ns,p999_ns,cores_used"
  for SPIN in 0 100 500 2000; do
    "$BIN/ge_loadgen" --clients 100 --client-threads 2 --io-threads 2 --shards 2 \
        --ping-rate 50 --move-rate 0 --duration "$DUR" --warmup "$WARM" --spin-us "$SPIN" \
        --json "$OUT/spin-$SPIN.json" > /dev/null 2>&1
    python3 - "$OUT/spin-$SPIN.json" "$SPIN" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); l=d["latency"]["ping_wire"]; c=d["cpu"]
print("%s,%d,%d,%d,%.3f"%(sys.argv[2],l["p50_ns"],l["p99_ns"],l["p999_ns"],c["cores_used"]))
PY
  done
} | tee "$OUT/spin-trade.csv"

echo
echo "=== 5/6  offered-load sweep (latency versus throughput) ==="
"$BIN/ge_loadgen" --clients 200 --client-threads 3 --io-threads 3 --shards 3 \
    --move-rate 0 --duration "$DUR" --warmup "$WARM" --spin-us 500 \
    --sweep 25,50,100,200,400,800,1600 --sweep-csv "$OUT/sweep.csv" \
    | strip_ansi > "$OUT/sweep.txt"
sed -n '/LOAD SWEEP/,$p' "$OUT/sweep.txt"

echo
echo "=== 6/6  thread scaling at saturating load ==="
# Varies only the server's thread count at a fixed offered load, and does it with
# spinning DISABLED. That matters: with an adaptive spin window, every extra
# thread burns its own spin budget whether or not it has work, so CPU-per-message
# climbs with thread count for a reason that has nothing to do with contention
# and the experiment measures the spin policy instead of the scaling. With
# blocking waits, a per-message cost that stays flat as threads are added is
# evidence there is no shared bottleneck; one that climbs points at a contended
# cache line, lock or allocator.
{
  echo "io_threads,shards,achieved_msgs_per_s,p50_ns,p99_ns,cpu_us_per_msg,cores_used"
  for N in 1 2 4 6; do
    "$BIN/ge_loadgen" --clients 200 --client-threads 3 --io-threads "$N" --shards "$N" \
        --ping-rate 400 --move-rate 0 --duration "$DUR" --warmup "$WARM" --spin-us 0 \
        --json "$OUT/scale-$N.json" > /dev/null 2>&1
    python3 - "$OUT/scale-$N.json" "$N" <<'PY2'
import json,sys
d=json.load(open(sys.argv[1])); l=d["latency"]["ping_wire"]; c=d["cpu"]; t=d["throughput"]
print("%s,%s,%.0f,%d,%d,%.3f,%.3f"%(sys.argv[2],sys.argv[2],t["achieved_msgs_per_s"],
      l["p50_ns"],l["p99_ns"],c["cpu_us_per_message"],c["cores_used"]))
PY2
  done
} | tee "$OUT/scaling.csv"

./scripts/machine_state.sh | awk '{printf "%s free cores of %s (%s%% idle, load average %s)\n", $1,$2,$3,$4}' | tee "$OUT/machine-state-after.txt"

echo
echo "=== writing docs/RESULTS.md ==="
python3 scripts/make_results.py "$OUT" > docs/RESULTS.md
echo "done -- docs/RESULTS.md, raw data in $OUT/"
