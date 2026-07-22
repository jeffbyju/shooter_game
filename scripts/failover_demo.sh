#!/usr/bin/env bash
# Kills a backend mid-run and shows the gateway eject it and keep serving.
#
# What to look for: `ejections 1` on the killed backend, sessions continuing to
# be proxied afterwards, and the load generator reporting disconnects only for
# the sessions that were on the dead node.
#
# Written for bash 3.2, which is what ships on macOS -- no negative array
# subscripts, no associative arrays.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-build}
LOG=${LOG:-/tmp/ge-failover}
mkdir -p "$LOG"

B1=""; B2=""; GW=""; LOAD=""
cleanup() { for p in $LOAD $GW $B1 $B2; do kill -TERM "$p" 2>/dev/null; done; }
trap cleanup EXIT

echo "== starting two backends =="
$BIN/ge_world_server --port 9201 --io-threads 1 --shards 1 --quiet >"$LOG/b1.log" 2>&1 & B1=$!
$BIN/ge_world_server --port 9202 --io-threads 1 --shards 1 --quiet >"$LOG/b2.log" 2>&1 & B2=$!
sleep 1

echo "== starting gateway (probes every 300ms, ejects after 3 consecutive failures) =="
$BIN/ge_gateway --port 9200 --threads 1 --probe-ms 300 \
    --backend 127.0.0.1:9201 --backend 127.0.0.1:9202 >"$LOG/gw.log" 2>&1 & GW=$!
sleep 1

echo "== 40 clients for 12s; backend 9201 dies at t+6s =="
$BIN/ge_loadgen --host 127.0.0.1 --port 9200 --clients 40 --client-threads 2 \
    --ping-rate 50 --move-rate 20 --duration 10 --warmup 2 >"$LOG/load.log" 2>&1 & LOAD=$!

sleep 6
echo "== kill -9 backend 9201 =="
kill -9 "$B1" 2>/dev/null; B1=""

wait "$LOAD" 2>/dev/null; LOAD=""
kill -TERM "$GW" 2>/dev/null; sleep 1

echo
sed 's/\x1b\[[0-9;]*m//g' "$LOG/gw.log" | tail -10
echo
echo "== client-visible effect =="
sed 's/\x1b\[[0-9;]*m//g' "$LOG/load.log" | grep -E "client disconnects|schedule fidelity|server -> client"
echo
echo "full logs in $LOG"
