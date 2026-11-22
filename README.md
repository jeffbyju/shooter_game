# ge_arena_udp

A first-person arena shooter server in C++23, written to find out where the
time actually goes when you serve a few thousand players from one machine —
and then to prove it with measurements rather than assert it.

Players run, jump, fly with jetpacks and shoot each other. One core serves
2,048 of them at a sustained 60 Hz with full snapshot delivery; eight cores,
placed correctly, reach **950,687 datagrams/s**.

No third-party dependencies. CMake and a C++23 compiler. Verified on Linux
(epoll, GCC 14) and macOS (kqueue, Apple clang 17).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Play it

```sh
build/ge_arena_udp --port 9200 --world 400 --aoi 64 &
build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp --bots 8 --seconds 600 &
build/ge_shooter --host 127.0.0.1 --port 9200 --udp
```

WASD and mouse to move and look, space to jump, double-tap and hold space for
the jetpack, click to fire. Cyan pickups are fuel, amber are ammo.

The client needs SDL3 and the macOS OpenGL framework; without them CMake skips
it and the bots still work.

## See the measurement that mattered, on your own machine

The original area-of-interest loop is still in the binary behind a flag, so the
A/B runs from **one build** and a difference cannot be a build difference.

```sh
build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048 &
build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp --bots 256 --seconds 10
```

Watch `aoicand/tick` in the server's per-second line, then restart it with
`--aoi-scan` and run the bots again. On a laptop that is **808 against
65,280** — and 256 × 255 = 65,280, which is what a full scan does by
definition. About a minute, no cloud account.

## How it works

One UDP socket per receive thread, all bound to the same port with
`SO_REUSEPORT`. The kernel hashes each client's 4-tuple to a fixed socket, so a
player stays on one thread for its whole session and that thread is the **only
writer** of its state. The world is shared, so the tick is split by a barrier:

```
  all threads   drain own socket until the tick deadline
  ─────────────────────── barrier ───────────────────────
  thread 0      apply queued joins, step the world, rebuild the AOI grid
  ─────────────────────── barrier ───────────────────────
  all threads   build and send snapshots for their own peers
  ─────────────────────── barrier ───────────────────────
  thread 0      evict silent peers, fold counters, report
```

Inputs are applied during the drain phase because each thread only touches its
own players' entities. Joins mutate the arena and the peer table, so they are
parked per-thread and applied by thread 0 between barriers. **There is not one
mutex in the server** — only the barrier's two atomics.

A datagram's path, end to end:

```
 1  client sends 48 bytes              ShooterInput, UDP
 2  kernel hashes the 4-tuple          -> socket k, thread k
 3  rx.recv(myfd)                      recvmmsg, up to 64 at once
 4  FrameReader::next()                magic, version, length, type
 5  peer_table.find(addr)              -> peer slot, O(1)
 6  arena.set_input(entity, ...)       writes ONLY this client's entity
 7  [barrier] simulate: movement, hitscan, pickups, rebuild grid
 8  [barrier] aoi_grid.query_aabb()    visit only nearby cells
 9  exact distance test                the grid filters, it does not decide
10  encode up to 32 PlayerRecords, tx.commit()
11  tx.flush(myfd)                     sendmmsg, up to 64 datagrams
```

Steps 3–6 and 8–11 run on every thread at once; step 7 runs on one. **The
syscalls parallelise and the simulation does not**, which is the whole scaling
story.

### The structures, and why they are shaped that way

**`ArenaStore` is struct-of-arrays.** `x_[]`, `y_[]`, `z_[]`, `health_[]` as
separate contiguous arrays. `step()` touches position and velocity and nothing
else, so every cache line it loads is entirely useful.

**`SpatialGrid` is compressed sparse row.** One counting pass, a prefix sum
into cell offsets, one scatter into a flat index array — not a
vector-of-vectors. Two linear passes over contiguous memory, zero allocation
after the first tick.

**`PeerTable` rebuilds on erase** rather than using tombstones. Eviction
happens at most once a tick; a lookup happens 200,000 times a second.

**Entities are a dense array with swap-remove**, so iteration is always over
`[0, size)` with no gaps. An entity's index changes under it, which is why
`Peer::arena_slot` exists and is fixed up on every removal.

**Frames are fixed-size and 8-byte aligned.** A `PlayerRecord` is exactly 32
bytes, with `fuel` and `ammo` in bytes that were already padding. Decoding is a
bounds check and a cast — no parsing, no allocation.

### Deliberately absent

- **No allocation after startup.** The test suite asserts zero steady-state
  allocations while serving.
- **No per-peer receive buffer.** A datagram is a whole message, so there is no
  reassembly state — which is what would make a kernel-bypass transport a
  drop-in replacement.
- **No retransmission, acks or ordering.** Losing a snapshot costs one frame of
  staleness, because the next one is self-contained. A protocol with no retries
  cannot build a retry storm.

## Measurements

All on `c7g.2xlarge` (Graviton3, 8 cores) with load from separate EC2
instances, never co-located with the server. `out / owed` is what the server
emitted against what it owes — `players × 60 × 2`, since each client is sent a
snapshot and a pong per tick. That ratio is internal to the server, so a
saturated load generator cannot forge it.

### One core

| | |
|---|---|
| players | 2,048 at a sustained 60 Hz |
| snapshot delivery | 100.00% |
| datagrams in / out | 249,859 /s and 249,858 /s |
| server CPU | ~99% of one core |
| 10-minute soak | RSS flat at 4,544 KB, every error counter 0 |

Of the 7,305 instructions each datagram costs, **6,091 are kernel**. Simulation
is 15.2% of the core. The syscall path is what is full, and every optimisation
below follows from that one measurement.

### Area of interest, isolated by a runtime flag

Median of 3 runs, `[min–max]`:

| | `--aoi-scan` | grid |
|---|---|---|
| distance checks / tick | 4,262,127 `[±0]` | **39,517** `[39.2–40.1k]` |
| instructions retired | 188.44 B | **91.26 B** |
| simulation CPU | 36.9% | **15.2%** |

2048 × 2047 = 4,192,256 is what a full scan does by definition, and that is
what the counter reads. The loop was provably quadratic, not merely slow.

It is also visible in a profile, but only if you ask correctly. Grouped by
function the whole server is one `main` box worth 16.89% and the loop cannot be
named at all, because inlining deleted its stack frame. Grouped by source line:

| | `--aoi-scan` | grid |
|---|---|---|
| `main.cpp:611-618` | **10.57%** | **0.00%** |
| all user-space samples | 16.84% | 7.18% |

Two thirds of all user-space time on four lines, invisible in the default view.

### Scaling across cores

| configuration | players at 95%+ served | peak out /s | cores |
|---|---|---|---|
| 1 thread | 4,096 | 468,000 | 1.00 |
| 4 threads | 7,168 | 820,296 | 3.65 |
| 8 threads | ~8,192 | 908,363 | 7.66 |
| **6 threads + 2 cores for NIC interrupts** | **~6,100 at 100%** | **950,687** | 8 |

The best configuration uses **fewer** worker threads than the row above it.
Interrupt placement is worth more than two more threads.

Fitting Amdahl to the unpinned runs gives a non-scaling fraction of 42.7% at
four threads and 44.6% at eight — two independent points on one number. But the
placed result reaches 2.03× on six threads where that model predicts 1.88×, so
the 44% had bundled interrupt contention into the "serial" fraction. Refitted,
39.1% and a ceiling of 2.56×. **An Amdahl fit measures whatever is not scaling
today, including what you could have fixed.**

### Interrupt placement

The NIC has eight receive queues, each steerable via `/proc/irq/<n>/smp_affinity`.
At 8,192 clients:

| arm | threads | IRQ cores | out /s | softirq cores |
|---|---|---|---|---|
| spread | 8 | all 8 | 793,609 | 1.44 |
| reserve 1 | 7 | core 0 | **555,669** | **0.92** |
| reserve 2 | 6 | cores 0–1 | **950,687** | 1.36 |

**Reserving one core is the worst option available** — worse than doing
nothing. Eight queues pinned to one core cap softirq at 0.92 cores while the
work wants 1.4, so the receive path throttles and packets the server could have
answered never reach it. The ordering is `2 > 0 > 1`, non-monotonic, so one
data point would have given the wrong rule.

Softirq is also ~25% more productive on dedicated cores (699k datagrams/s per
softirq core, against 551k when sharing with the game loop).

### Past the knee

One thread plateaus: 452,605 /s at 4,096 clients and 452,599 /s at 6,144 — the
same number twice under 50% more load. Four threads peak at 820k and then
*fall*: 799k, 653k, 562k. A barrier costs what its slowest arrival costs, and
overload makes arrivals uneven, so every tick waits for the worst-off thread.
**The synchronisation that makes it faster below the knee makes it fail worse
above it.**

All 6,144 clients still join at 3× the saturating load, with no malformed
frame, no failed send and no sequence gap. It sheds quality, not service.

### Over the real internet

256 clients from each of us-east-2, us-west-2 and eu-west-1 against one server:

| origin | delivery | RTT p50 | inputs unanswered |
|---|---|---|---|
| same VPC | 99.5% | 16.7 ms | 0.02% |
| us-west-2 | 99.2% | 66.7 ms | 0.07% |
| eu-west-1 | 99.5% | 83.3 ms | 0.11% |

`bad 0  sendfail 0  nosession 0  inputgaps 0  rewelcome 0` for the whole run.
The hello retry and stale-snapshot guard were written against *injected* loss
because a loopback cannot produce the real thing; this is the first run that
put real loss in front of them.

RTT is quantised to one 60 Hz tick, because a bot reads its socket on its own
send cadence. These figures separate continents; they are not network round
trips.

### A server can keep up and still not deliver

At 6,144 clients the server emitted 737,281 /s against 737,280 owed — 100.0% —
while clients received **69.8%**.

```
ethtool -S ens5 | grep allowance
pps_allowance_exceeded   45,997,437 -> 47,521,425   = 47,625 /s discarded
```

EC2 shapes on **packet rate**, not bandwidth. The hypervisor was discarding
~47,600 packets/s beyond the instance's allowance, after the kernel had
accepted them — at 3.4 Gbps of a nominal 15. That accounts for 6.5% of the
30% gap; the rest is the load generators' own receive paths.

This is why the protocol counts packets rather than bytes. One datagram per
player per tick at 60 Hz is 122 packets/s/player, and a quarter of a million
packets a second arrives long before bandwidth is anywhere near saturated.

### Realistic capacity

| | players | 64-player matches |
|---|---|---|
| lab peak, 100% CPU, uniform spread | 7,922 | 124 |
| fully served | 6,100 | 95 |
| **70% CPU + combat clustering** | **~4,400** | **~69** |

The last row is the one to quote. Production does not run at 100% CPU, and real
players cluster into chokepoints where uniformly-spread bots do not. The 20%
clustering derate is an estimate, not a measurement.

The cheapest way to double it is not more machines: every player costs 120
datagrams/s because pings are answered separately from snapshots. Fold the pong
into the snapshot and the same box holds ~8,900 players.

## Running it properly

```sh
# the server
build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048 [--rx-threads N]

# load, from a different machine
build/ge_shooter_bots --host SERVER --port 9200 --udp --bots 2048 --seconds 60 --json

# A/B the two optimisations that matter, from one binary
build/ge_arena_udp --aoi-scan    # the original O(n^2) area-of-interest loop
build/ge_arena_udp --batch 1     # one recvfrom/sendto per datagram

# tests and microbenchmarks
build/ge_tests                   # 94 tests, 312k assertions
build/ge_bench
```

`--rx-threads > 1` **requires Linux.** The design relies on the kernel hashing
a peer's 4-tuple to a fixed socket. macOS accepts `SO_REUSEPORT` and then
spreads one peer across sockets, which puts two threads on one peer record —
found by ThreadSanitizer, and a correctness bug rather than a slowdown. The
server refuses rather than pretending.

For the best configuration, pin the NIC's interrupts and keep threads off those
cores:

```sh
for i in $(seq 56 63); do echo 3 | sudo tee /proc/irq/$i/smp_affinity; done
taskset -c 2-7 build/ge_arena_udp --rx-threads 6 ...
```

## Layout

```
apps/arena_udp/        the server: sockets, threads, tick loop  (~900 lines)
apps/shooter_client/   SDL3 + OpenGL client, and the shared NetClient
apps/shooter_bots/     headless clients: functional test and load generator
include/ge/proto/      wire structs with compile-time invariants, zero-copy codec
include/ge/world/      SoA entity store, CSR spatial grid, pickups
include/ge/core/       monotonic clock, PRNG, thread naming
include/ge/metrics/    HDR histogram, per-thread counters
tests/                 94 tests: protocol + mutation fuzzing, grid, arena,
                       allocators, client linear algebra
results/               raw output behind every number above
```

The repository also contains `ge_world_server` (a sharded TCP server) and
`ge_gateway`, from an earlier snake game that shares this protocol and codec.
They are not what the measurements above are about.

## What is in `results/`

Raw server logs, generator JSON and the orchestrator script for each campaign,
verbatim.

| directory | campaign | the gate, and the caveat that matters |
|---|---|---|
| `perf/` | area-of-interest A/B, both profile groupings | 0 lost samples; `--aoi-scan` makes it one binary |
| `scale/` | single-core capacity ramp | generator idle never below 44% |
| `ceiling/` | threaded ceiling, 1/4/8 threads | idle never below 42.7%; single 45 s runs, knee bracketed not located |
| `placement/` | interrupt affinity and softirq | `ethtool` before/after included; receive and transmit softirq share one counter |
| `reuseport/` | first `--rx-threads` run | **superseded by `ceiling/`** — its generators sat at 1–3% idle, so its delivery column is the rig's limit, not the server's |
| `geo/`, `xcontinent/` | clients on three continents | RTT quantised to one tick |
| `world-server/` | the sharded TCP server, for comparison | an earlier run of it was never archived |

Two numbers above are **not** backed by anything in `results/`: the original
`perf` srcline percentages (20.9/6.2/13.4%), whose `perf.data` was not kept and
whose pre-fix build no longer exists in git, and the first `ge_world_server`
run. Everything else can be recomputed from those directories.

## Known limitations

- **No client prediction, interpolation or lag compensation.** All three are
  standard in shipped games. Their absence is what makes the latency numbers
  honest, and also why it feels worse than a commercial title. Interpolation is
  the biggest available quality win — roughly 80 lines, client-only.
- **No authentication, encryption or anti-cheat.** The server trusts everything
  it is told except movement speed. Not production-ready.
- **One server process, one region.** No failover, no persistence, no
  matchmaking. Nothing survives a restart.
- **No kernel bypass and no busy-poll.** `SO_REUSEPORT` took the syscall path as
  far as threads go; the remaining kernel cost needs a different transport.
- **Interrupt affinity is not set by the server.** The best configuration needs
  it done by hand, and the default spread is ~17% worse.
- **Branch misses doubled** at 2,048 after the AOI fix (28.1 M → 58.2 M).
  Plausibly hash probing replacing a perfectly predicted linear scan —
  **unverified**, so not asserted.
- **Latency is quantised to one tick**, so no RTT finer than 16.67 ms can be
  resolved.
- **Ceiling and placement cells are single 45 s runs**, not medians. The shapes
  are robust; individual figures are not tight.
- **Endianness is not handled.** Little-endian is assumed everywhere.

## Next steps

- Cut packets per player: fold the pong into the snapshot (2×), then drop
  distant players to 30 or 20 Hz (4–6×)
- Ship the interrupt placement instead of requiring it by hand
- Snapshot interpolation on the client
- Real io_uring: the current backend uses `IORING_OP_POLL_ADD` only, so it
  learns which sockets are ready and then does ordinary reads and writes.
  Batched `RECV`/`SEND`, SQPOLL and multishot are unimplemented
- Kernel bypass, since 83% of the per-datagram cost is kernel
- A fleet manager rather than an autoscaling group: game servers are stateful,
  so scale-in has to drain before it terminates
