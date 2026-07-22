#!/usr/bin/env bash
# Starts a server configured for a shared, watchable snake game and prints the
# command each player should run.
#
# The defaults of ge_world_server are tuned for benchmarking, not for playing
# together, and two of them will leave every player alone in an empty world:
#
#   --shards N   players are split across shards by session hash and shards
#                cannot see each other, so this must be 1
#   --aoi R      how far a player can see; smaller than the world means two
#                players are almost never in range
#
# A small world also matters: snakes have to be able to find each other.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-build}

PORT=${PORT:-9800}
WORLD=${WORLD:-110}
TICK=${TICK:-30}
BOTS=${BOTS:-6}

if [ ! -x "$BIN/ge_world_server" ]; then
  echo "no build found at $BIN -- run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

SRV=""
cleanup() { [ -n "$SRV" ] && kill -TERM "$SRV" 2>/dev/null; }
trap cleanup EXIT

"$BIN/ge_world_server" --port "$PORT" --shards 1 --world "$WORLD" \
    --aoi "$((WORLD * 2))" --tick-hz "$TICK" --max-conns 64 --quiet &
SRV=$!
sleep 1

if ! kill -0 "$SRV" 2>/dev/null; then
  echo "server failed to start (is port $PORT already in use?)" >&2
  exit 1
fi

cat <<TXT

  Snake server running on 127.0.0.1:$PORT
    one shard, ${WORLD}x${WORLD} world, everyone can see everyone, ${TICK} Hz

  Open three more terminals and run ONE of these in each:

      $PWD/$BIN/ge_viewer --host 127.0.0.1 --port $PORT --bots 0

  Steer with WASD or the arrow keys. Run your head into another snake's body
  and you die; they grow by half your length. Walls kill too. You respawn after
  about a second.

  To add some computer-controlled snakes for company, give ONE of the players
  --bots $BOTS instead of --bots 0.

  Ctrl-C here stops the server.

TXT

wait "$SRV" 2>/dev/null
