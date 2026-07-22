#!/usr/bin/env bash
# Provision a fresh Linux VM, build, validate, benchmark, and shut down.
#
# Designed so an instance's life is minutes rather than however long you take to
# remember it exists. Pass it as EC2 user-data, or run it after ssh-ing in.
#
#   REPO=https://github.com/you/game_engine_server.git \
#   S3_BUCKET=my-bucket SHUTDOWN=1 ./bootstrap.sh
#
# Environment:
#   REPO        git URL to clone (required unless run inside a checkout)
#   S3_BUCKET   optional; results are uploaded here
#   SHUTDOWN    set to 1 to power off when finished (use with
#               instance-initiated-shutdown-behavior=terminate)
set -euo pipefail

REPO=${REPO:-}
S3_BUCKET=${S3_BUCKET:-}
SHUTDOWN=${SHUTDOWN:-0}
OUT=${OUT:-$HOME/ge-results}

log() { printf '\n=== %s ===\n' "$*"; }

log "machine"
uname -a
lscpu 2>/dev/null | grep -E "^(Architecture|CPU\(s\)|Thread|Core|Socket|Model name|NUMA)" || true
free -h 2>/dev/null | head -2 || true

log "toolchain"
if command -v apt-get >/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y -qq --no-install-recommends g++ cmake make git ca-certificates
elif command -v dnf >/dev/null; then
  sudo dnf install -y -q gcc-c++ cmake make git
fi
g++ --version | head -1
cmake --version | head -1

if [ -n "$REPO" ]; then
  log "source"
  rm -rf "$HOME/ge" && git clone --depth 1 "$REPO" "$HOME/ge"
  cd "$HOME/ge"
fi

log "build"
# -DGE_NATIVE_ARCH=ON is right here, unlike in CI: this IS the machine being
# measured, so tuning for it is the point.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGE_NATIVE_ARCH=ON > /dev/null
cmake --build build -j"$(nproc)"

log "correctness"
./build/ge_tests

log "environment as the harness sees it"
# Recorded before any measurement: it is what makes the numbers interpretable.
./build/ge_loadgen --clients 8 --client-threads 1 --duration 2 --warmup 1 2>&1 |
  sed 's/\x1b\[[0-9;]*m//g' | sed -n '1,14p'

log "hardware counters"
# Most VMs have no passthrough PMU. Lowering paranoid costs nothing if they do.
sudo sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true
./build/ge_loadgen --clients 40 --duration 6 --warmup 2 2>&1 |
  sed 's/\x1b\[[0-9;]*m//g' | sed -n '/HARDWARE COUNTERS/,/^$/p'

log "io_uring backend (UNVALIDATED path -- this is the point of the exercise)"
KERNEL=$(uname -r)
echo "kernel $KERNEL"
for BACKEND in poll uring; do
  echo "--- $BACKEND ---"
  ./build/ge_loadgen --io-backend "$BACKEND" --clients 50 --client-threads 2 \
      --duration 6 --warmup 2 2>&1 | sed 's/\x1b\[[0-9;]*m//g' |
    grep -E "io backend|ping RTT \(wire|frames per read|frames per write|parse errors|verdict" -A1 |
    head -14 || echo "  FAILED -- see docs/tutorial/14 for the known weaknesses"
done

log "full benchmark suite"
scripts/run_bench.sh full || true

log "collecting"
mkdir -p "$OUT"
cp -r docs/results docs/RESULTS.md "$OUT"/ 2>/dev/null || true
tar czf "$HOME/ge-results.tgz" -C "$HOME" "$(basename "$OUT")"
echo "results at $HOME/ge-results.tgz"

if [ -n "$S3_BUCKET" ]; then
  aws s3 cp "$HOME/ge-results.tgz" "s3://$S3_BUCKET/" || echo "upload failed; copy it manually"
fi

if [ "$SHUTDOWN" = "1" ]; then
  log "shutting down"
  sudo shutdown -h now
fi
