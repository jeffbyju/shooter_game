# ge_server

Two low-latency multiplayer game servers in C++23, sharing one protocol,
one codec and one set of containers.

- **`ge_arena_udp`** — a first-person arena shooter over UDP, served from a
  single core. 2048 players at a sustained 60 Hz.
- **`ge_world_server`** — a sharded TCP server for the snake mode, acceptor
  to I/O threads to simulation shards over lock-free rings.

Built the way a trading system is built: fixed capacity everywhere, no
allocation on the hot path, lock-free handoff between threads, and a benchmark
harness that reports where every microsecond goes and refuses to flatter itself.

No third-party dependencies. CMake plus a C++23 compiler. Verified on macOS
(kqueue, Apple clang 17) and Linux (epoll, GCC 13).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

The arena shooter needs a server and something to play against:

```sh
build/ge_arena_udp --port 9200 --world 400 --aoi 64 &
build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp --bots 8 --seconds 60 &
build/ge_shooter --host 127.0.0.1 --port 9200 --udp
```

WASD and mouse, space to jump, double-tap and hold space for the jetpack,
click to fire. Cyan powerups are fuel, amber are ammo.

To see the measurement that mattered, on your own machine, in about a minute:

```sh
build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048 &
build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp --bots 256 --seconds 10
```

Watch `aoicand/tick` in the server's per-second line, then restart it with
`--aoi-scan` and run the bots again. The flag restores the original O(n^2)
area-of-interest loop, so both arms are the same binary. On a laptop that is
808 against 65,280, and 256 x 255 = 65,280.

```sh
build/ge_tests                    # 94 tests, 312k assertions
build/ge_bench                    # component microbenchmarks
build/ge_loadgen --clients 100    # end-to-end benchmark, server runs in-process
build/ge_viewer                   # the snake mode, server + 24 bots in the terminal
```

`ge_viewer` is the quickest way in. It starts a server, connects a crowd of
snakes, and draws one player's view of the world. Run your head into another
snake's body and you die; they grow.

```
ge_snake  tick 353  sim 30.0 Hz  snakes 11/15  len 14 kills 2   input->view 15.4 ms
+------------------------------------------------------------------------------+
|        Oo                                                                    |
|         ooo                                             oooooooo             |
|           o                                          Ooo        ooo          |
|                          ooooooo                              oO             |
|                     ooooo       oooooo           oo                          |
|                    oo          @**    ooooooooo    o                         |
|                      ****   ooo                                              |
+------------------------------------------------------------------------------+
@ you  * your body  O rivals  o their bodies   run into a body and you die
```

It is a real client, not a debug window: it speaks the wire protocol over TCP
and renders only what the server chose to send it.

## Architecture

```
clients --> ge_gateway --> ge_world_server xN
                              acceptor -> I/O threads -> simulation shards
                                        (lock-free rings both directions)
```

`ge_world_server` accepts clients, runs a fixed-rate simulation sharded across
threads, and streams each client an area-of-interest view of the world.
`ge_gateway` spreads sessions across world servers with consistent hashing,
load-aware placement and outlier ejection. `ge_loadgen` offers open-loop load
and measures the result.

The design constraint is that there is **no mutex on any steady-state path**.
Each shard owns its entity arrays exclusively, so the tick body has nothing to
lock and no possibility of a torn read. Threads communicate only through bounded
single-producer/single-consumer rings.

### Zero-copy wire protocol

Messages are fixed-layout PODs cast in place from the socket buffer. There is no
parse step and no allocation. The cast's alignment precondition is enforced as a
protocol invariant: every frame length must be a multiple of 8 and the parser
rejects anything else, so a hostile peer cannot desynchronise alignment.

A `consteval` check proves every message tag maps to exactly one payload struct,
which makes an unhandled message type a build error rather than a production log
line.

### Zero allocations, measured

The build interposes global `operator new` and `delete` and counts every call on
per-thread, cache-line-isolated counters. A 10 second run at 100 clients reports
**0 steady-state allocations**, 131 for the entire process lifetime. Memory is a
function of configuration rather than load: every buffer, queue, pool and entity
array is fixed-capacity and mapped at startup, at 20.1 KiB per connection.

### Bounded everything

Every queue has a fixed capacity, so overload is shed and counted rather than
absorbed into unbounded memory. Command ring full, state deltas dropped, short
writes, slow clients evicted, parse errors, late ticks and skipped ticks are all
reported per run. There is no silent loss path.

### A harness that argues against itself

Load is generated **open-loop**: send times come from a schedule fixed before the
run, so the server cannot throttle its own offered load. Latency is measured from
when a request was *due*, not when it was sent, which makes the numbers
coordinated-omission-free. A test demonstrates that without this, a server
completely unavailable for one second still passes p99, p99.9 and p99.99,
appearing only as `max`.

Every run prints clock granularity, its own CPU use, and a `RUN VALIDITY` section
listing reasons to distrust it: major page faults, unpinned threads, insufficient
samples, undelivered offered load, and measured scheduler interference. The load
sweep refuses to count rows where the generator was CPU-bound.

## Measurements

Two separate campaigns. The arena server was measured on Linux hardware in EC2;
the component costs and the snake end-to-end figures come from a laptop. They
are not comparable and are kept apart deliberately.

### The arena server, one core

`c7g.2xlarge` (Graviton3), one core, 2048 clients on separate instances in the
same VPC, each client's sends spread across the tick rather than fired in
lockstep.

| | |
|---|---|
| players | 2048 at a sustained 60 Hz |
| snapshot delivery | 100.00% |
| datagrams in / out | 249,859 /s and 249,858 /s |
| server CPU | ~99% of one core |
| 10-minute soak | RSS flat at 4,544 KB, every error counter 0 |

The CPU figure is the interesting one, because it used to read ~70%. The load
generator was firing all 2048 clients at the same instant; bursty arrivals are
cheaper per datagram than smooth ones because one `poll` wake amortises over
many of them. Staggering the clients moved the identical 249,856 datagrams/s
from 77% of a core to 99%. The earlier number is withdrawn, not revised.

Of the 7,305 instructions each datagram costs, 6,091 are kernel. Simulation is
15.2% of the core; the syscall path is what is full.

Both optimisations that matter are behind runtime flags, so the A/B runs from
one binary and a difference cannot be a build difference. Median of three runs,
`[min-max]`:

| | `--aoi-scan` | grid |
|---|---|---|
| AOI distance checks / tick | 4,262,127 `[+-0]` | **39,517** `[39.2-40.1k]` |
| instructions retired | 188.44 B `[188.3-189.1]` | **91.26 B** `[91.2-91.6]` |
| simulation CPU | 36.9% `[36.3-37.5]` | **15.2%** `[15.1-16.0]` |
| snapshots out | 249,857 /s | 249,855 /s |

2048 x 2047 = 4,192,256 is what a full scan does by definition, which is what
the counter reads. Delivery is identical in both arms: the grid is worth 2.06x
in instructions, not a delivery win at this load.

Batching with `recvmmsg`/`sendmmsg` is worth ~10% of cycles at 2048 (49.78 B to
44.94 B) with no change in delivery. It pays at 4096, where snapshots out go
from 314,704 to 360,377 /s, +14.5%. The 4096 arms are generator-limited, so
they are a fair comparison between builds and not a capacity measurement.

### Component costs

Apple M-series, `-O3`, best of seven batches. The allocation counter reads zero
for every row except the two that exist to allocate.

| operation | cost |
|---|---:|
| encode a 32-byte frame | 0.75 ns |
| decode + type-checked view | 0.34 ns |
| decode + `visit` dispatch | 0.94 ns |
| encode a snake delta, 16 snakes x 32 points | 199 ns |
| decode it and walk all 512 points | 483 ns |
| SPSC ring push+pop, same thread | 7.54 ns |
| SPSC ring push, 48-byte command | 8.83 ns |
| byte ring write+peek+pop, 256B | 11.7 ns |
| slot map find, generation-checked | 0.68 ns |
| SPSC cross-core round trip | 90.0 ns |
| arena bump allocate | 0.93 ns |
| pool acquire + release | 2.36 ns |
| `malloc` + `free`, same size | 25.4 ns |
| `operator new` + `delete`, what is avoided | 39.1 ns |
| histogram record | 3.42 ns |
| `Clock::now_ns` | 9.99 ns |
| advance one snake, steer + move | 8.21 ns |
| spatial grid rebuild, per body point | 1.75 ns |
| area-of-interest query, radius 64 | 6.72 ns |

End to end, the snake path over TCP on the same laptop:

| | |
|---|---:|
| server-side service time, frame parsed to reply queued | **2.25 us** p50, 958 ns min |
| raw loopback `write`/`read` floor, same machine | 10.3 us min |
| sustained throughput | **78.7K msg/s** at p50 380 us on 2.3 cores |
| steady-state heap allocations | **0** |
| resident set / per connection | 11.7 MiB / 20.1 KiB |
| tests | 94 tests, ~312,600 assertions, clean under ASan+UBSan |

Clock overhead is 13 ns and granularity 41 ns on this machine, so batches are
sized such that neither matters. Any figure within a few multiples of 41 ns
would be instrument noise rather than signal.

> The committed benchmark run was collected on a contended laptop: load average
> rose from 7.8 to 27.8 on 10 cores during collection, and simulation ticks
> started 11.65 ms late at p99. p50, the server-side figures and the component
> microbenchmarks hold up. **The p99 and beyond columns in that run are scheduler
> noise, not server behaviour, and should not be quoted.** Re-run
> `scripts/run_bench.sh` on an idle machine for publishable tails.

## What the measurements changed

The first end-to-end run showed p50 = 163 us while the server's own service time
was 1.9 us. A one-way decomposition placed **121 us of it before the server saw
the bytes**: the I/O thread was waking from a deep idle core. Raising offered
load to 50k msg/s, which keeps cores hot, dropped p50 to 28 us with a 9.4 us
floor, confirming it. Optimising the 1.9 us handler would have bought about 1%.

That produced the adaptive spin option (`--spin-us`) and a results table showing
both sides of the trade rather than a single flattering number. It also produced
an honest caveat: on a contended machine, spinning threads compete with
everything else and the tail gets *worse*. Spinning pays only when you own the
cores.

The same sweep found three real defects that hand-testing had not: a heap
corruption when a `Disconnect` shared a TCP segment with later frames, a
connection slot-and-buffer leak on close, and a latent cross-thread
use-after-free caused by an object pool threading its free list through the
freed object's file descriptor. All three are fixed and each has a regression
test verified to fail without its fix.

Two of the instruments were wrong as well. `poll()` wait time was being counted
as work, and Darwin's involuntary context-switch counter turned out to track
blocking syscalls rather than preemption: a pure spin loop on an idle machine
baselines near 700/s while the counter reported 190,000/s. Scheduler
interference is now judged from measured tick jitter instead.

## Layout

```
include/ge/
  core/        platform facade, monotonic clock, thread naming/QoS, PRNG, topology
  alloc/       bump arena, object pool, global allocation probe
  container/   SPSC ring, SPSC variable-length byte ring, MPMC queue, slot map
  proto/       wire structs + compile-time invariants; zero-copy codec
  net/         socket options, kqueue/epoll/io_uring reactor, buffered connection
  world/       SoA entity store, CSR spatial grid, snake store, simulation shard
  lb/          consistent hash ring, P2C placement, outlier ejection
  metrics/     HDR histogram, counters, process resource usage, perf counters
  server/      world server: acceptor + I/O threads + shards
  bench/       load generator, report rendering
apps/          ge_world_server, ge_gateway, ge_loadgen, ge_viewer,
               ge_arena_udp, ge_shooter, ge_shooter_bots
bench/         component microbenchmarks
apps/arena_udp/       single-core UDP arena server
apps/shooter_client/  SDL3 + OpenGL client, and the shared NetClient
apps/shooter_bots/    headless clients: functional test and load generator
tests/         94 tests: containers, protocol + mutation fuzzing, allocators,
               histogram, balancer, grid, shard lifecycle, server lifecycle,
               arena shard, client linear algebra
scripts/       run_bench.sh, failover_demo.sh, make_results.py, snake_multiplayer.sh
```

## Usage

```sh
# Play against bots. WASD or arrows to steer, q to quit.
build/ge_viewer

# Multiplayer: starts a shared world and prints the command each player runs
scripts/snake_multiplayer.sh

# The arena shooter, single core over UDP
build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048

# ...and the load generator that doubles as its functional test
build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp --bots 512 --seconds 60

# A/B the two optimisations that matter, from one binary
build/ge_arena_udp --aoi-scan     # the original O(n^2) area-of-interest loop
build/ge_arena_udp --batch 1      # one recvfrom/sendto per datagram

# Standalone snake server
build/ge_world_server --port 7777 --io-threads 2 --shards 4

# Gateway across two backends
build/ge_gateway --port 8080 --backends 127.0.0.1:7777,127.0.0.1:7778

# Benchmark: in-process server, 200 clients, JSON report
build/ge_loadgen --clients 200 --duration 15 --json out.json

# Latency versus offered throughput
scripts/run_bench.sh

# Backend failure and recovery
scripts/failover_demo.sh
```

## Production-oriented features

- `SO_REUSEPORT` per-thread listeners, so the kernel load-balances accepts
  across I/O threads instead of funnelling them through one acceptor
- NUMA-aware CPU topology discovery and thread pinning, preferring physical
  cores before SMT siblings, with first-touch memory placement
- Hardware performance counters via `perf_event_open` (cycles, instructions,
  cache references and misses, branches and misses), reporting why they are
  unavailable rather than silently returning zeros
- An `io_uring` readiness backend selectable with `--io-backend`

## Known limitations

Scoped by campaign, because two of these used to be stated as if they applied
to everything.

The arena server:

- **No client prediction, interpolation or lag compensation.** All three are
  standard in shipped games. Their absence is what makes the latency numbers
  honest, and it is also why it feels worse than a commercial title.
  Interpolation is the largest available quality win and is client-only.
- **No authentication, encryption or anti-cheat.** The server trusts everything
  it is told except movement speed. Do not call it production-ready.
- **Single region, single instance type, one core.** No failover, no
  persistence. Nothing survives a restart.
- **No kernel bypass and no busy-poll.** No `SO_BUSY_POLL`, no AF_XDP, no DPDK.
  Given that 83% of the per-datagram cost is kernel, this is where the next win
  is, and it has not been done.
- **Branch misses doubled** at 2048 after the area-of-interest fix, 28.1 M to
  58.2 M. Hash probing and grid-cell iteration replacing a perfectly predicted
  linear scan is the plausible explanation. It is unverified, so it is not
  asserted.
- **LAN only.** Bots and server share a VPC. This measures the stack, not the
  internet.

The snake path and the component figures:

- **Laptop, unpinned, and the machine was contended.** See the caveat above the
  end-to-end table. p50 and the component microbenchmarks hold; p99 and beyond
  describe the machine.
- **Loopback.** No NIC, driver or switch in any of those numbers.
- **The `io_uring` path is unvalidated.** It compiles and is selectable but
  defaults to off and has never been run on a machine with io_uring. Note also
  that io_uring is not kernel bypass; the kernel TCP stack is still in the path.

Throughout:

- **Endianness is not handled.** Little-endian is assumed everywhere.
- **The gateway does not front the UDP path.**
- **Fuzzing is mutation-based, not coverage-guided**, and no sanitizers run in
  CI.
- **`include/ge/container/slot_map.hpp` is dead code with a test suite.**

## Next steps

- Cut syscalls, since that is where 83% of the per-datagram cost is: a
  `SO_REUSEPORT` receive thread per core first, then busy-poll, then AF_XDP
- Snapshot interpolation on the client, which is the biggest felt-quality gain
  available and is roughly 80 lines
- Re-collect the snake and component figures on idle, pinned Linux so the two
  campaigns stop being incomparable
- Validate the `io_uring` backend on a box that has it and benchmark it against
  epoll
- Spatial rather than session-hash sharding, so interacting entities share a
  shard -- worth doing only once the syscall path stops being the bottleneck,
  since simulation is currently 15% of a core
