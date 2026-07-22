#!/usr/bin/env bash
# Prints one line: "<free_cores> <total_cores> <idle_pct> <load1>"
#
# Load average is a bad contention signal on macOS: it counts threads blocked in
# uninterruptible waits, so a machine with 8 idle cores routinely reports a load
# average of 20+. What a benchmark actually needs to know is how many cores are
# free right now, so that is what this measures, with the load average kept only
# as supplementary context.
set -u

if [ "$(uname -s)" = "Darwin" ]; then
  CORES=$(sysctl -n hw.logicalcpu)
  IDLE=$(top -l 1 -n 0 2>/dev/null | awk -F'[ ,]+' '/^CPU usage/ {for(i=1;i<=NF;i++) if($i=="idle") print $(i-1)}' | tr -d '%')
  LOAD1=$(sysctl -n vm.loadavg | awk '{print $2}')
else
  CORES=$(nproc)
  # Two samples of /proc/stat one second apart; the delta is the real idle share.
  read -r _ u1 n1 s1 i1 w1 rest < /proc/stat
  sleep 1
  read -r _ u2 n2 s2 i2 w2 rest < /proc/stat
  BUSY=$(( (u2-u1) + (n2-n1) + (s2-s1) ))
  IDLED=$(( (i2-i1) + (w2-w1) ))
  TOTAL=$(( BUSY + IDLED ))
  IDLE=$(awk -v i="$IDLED" -v t="$TOTAL" 'BEGIN{printf "%.1f", t>0 ? 100.0*i/t : 100.0}')
  LOAD1=$(awk '{print $1}' /proc/loadavg)
fi

[ -z "${IDLE:-}" ] && IDLE=100
FREE=$(awk -v c="$CORES" -v i="$IDLE" 'BEGIN{printf "%.1f", c*i/100.0}')
echo "$FREE $CORES $IDLE $LOAD1"
