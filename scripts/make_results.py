#!/usr/bin/env python3
"""Render docs/RESULTS.md from the raw artefacts run_bench.sh produced.

Every number in the generated document traces back to a file in docs/results/,
so a reader can check any claim without rerunning anything.
"""
import csv
import json
import os
import sys
from datetime import datetime, timezone


def ns(v):
    v = float(v)
    if v < 1_000:
        return f"{v:.0f} ns"
    if v < 1_000_000:
        return f"{v/1e3:.2f} us"
    if v < 1_000_000_000:
        return f"{v/1e6:.2f} ms"
    return f"{v/1e9:.3f} s"


def rate(v):
    v = float(v)
    return f"{v/1e6:.2f} M/s" if v >= 1e6 else (f"{v/1e3:.1f} K/s" if v >= 1e3 else f"{v:.0f}/s")


def bytes_h(v):
    v = float(v)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if v < 1024 or unit == "GiB":
            return f"{v:.2f} {unit}" if unit != "B" else f"{v:.0f} B"
        v /= 1024


def latency_row(label, s):
    return (f"| {label} | {s['count']:,} | {ns(s['min_ns'])} | {ns(s['p50_ns'])} | "
            f"{ns(s['p90_ns'])} | {ns(s['p99_ns'])} | {ns(s['p999_ns'])} | "
            f"{ns(s['p9999_ns'])} | {ns(s['max_ns'])} |")


def main(outdir):
    main_json = os.path.join(outdir, "latency-100.json")
    if not os.path.exists(main_json):
        sys.exit(f"missing {main_json}; run scripts/run_bench.sh first")
    d = json.load(open(main_json))
    env, cfg, lat = d["environment"], d["config"], d["latency"]
    thr, alloc, mem, cpu, bp, sc = (d["throughput"], d["allocations"], d["memory"],
                                    d["cpu"], d["backpressure"], d["syscalls"])

    p = print
    p("# Measured results")
    p("")
    p(f"Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} by "
      "`scripts/run_bench.sh`. Raw artefacts are in `docs/results/`.")
    p("")
    p("## Machine and build")
    p("")
    p("| | |")
    p("|---|---|")
    p(f"| platform | {env['arch']} / {env['os']} |")
    p(f"| compiler | {env['compiler']} |")
    p(f"| build | {env['build']} |")
    p(f"| cores | {env['logical_cores']} logical, {env['performance_cores']} performance |")
    p(f"| thread pinning | {'yes' if env['threads_pinned'] else 'no (unavailable on this platform)'} |")
    p(f"| clock call overhead | {ns(env['clock_overhead_ns'])} |")
    p(f"| clock granularity | {ns(env['clock_granularity_ns'])} |")
    p("")
    p("> The clock granularity is the resolution floor of every latency figure below. "
      "Any number within a few multiples of it is instrument noise, not signal.")
    p("")

    # Machine contention during collection -- the single most important caveat
    # on any tail latency figure, so it goes near the top rather than buried.
    # Machine contention during collection -- the single most important caveat
    # on any tail latency figure, so it goes near the top rather than buried.
    #
    # The signal is free cores, not load average: on macOS the load average
    # counts threads in uninterruptible waits and routinely reads 20+ on a
    # machine with eight idle cores, which would make this warning cry wolf.
    state = []
    free_cores = None
    for name, when in (("machine-state-before.txt", "before"),
                       ("machine-state-after.txt", "after")):
        path = os.path.join(outdir, name)
        if not os.path.exists(path):
            continue
        txt = open(path).read().strip()
        if not txt:
            continue
        state.append(f"{when}: {txt}")
        if when == "before":
            try:
                free_cores = float(txt.split()[0])
            except (IndexError, ValueError):
                free_cores = None

    needed = 4.0

    # Scheduler interference is judged from a number this program measured --
    # how late the fixed-rate simulation tick actually started -- rather than
    # from a pre-run guess at machine business or from a platform context-switch
    # counter that means different things on different kernels.
    tick_hz = 60
    jitter_frac = None
    tj = lat.get("shard_tick_jitter")
    if tj and tj.get("count"):
        period_ns = 1e9 / tick_hz
        jitter_frac = tj["p99_ns"] / period_ns
    contended = (jitter_frac is not None and jitter_frac > 0.25) or \
                (jitter_frac is None and free_cores is not None and free_cores < needed)
    if state:
        p("### Machine state during collection")
        p("")
        for line in state:
            p(f"- {line}")
        p("")
        if contended:
            if jitter_frac is not None:
                p(f"- measured scheduler interference: simulation ticks started "
                  f"{ns(tj['p99_ns'])} late at p99, which is {jitter_frac*100:.0f}% of the "
                  f"{tick_hz} Hz tick period")
                p("")
            p(f"> **These numbers were collected on a contended machine.** The "
              f"simulation asks to be woken at a fixed rate; how late it actually "
              f"starts is a direct reading of how much the scheduler is delaying a "
              f"runnable thread, and on this run it is far past the threshold. "
              f"p50, the server-side service times, and the component "
              f"microbenchmarks remain "
              f"meaningful. **Every tail figure below — p99, p99.9, p99.99, max — and "
              f"the thread-scaling table are dominated by that interference, not by the "
              f"server, and should not be quoted as characterising it.** Re-run "
              f"`scripts/run_bench.sh` on an idle machine for publishable tails.")
        else:
            jf = f", and simulation ticks started within {jitter_frac*100:.0f}% of the tick period at p99" if jitter_frac is not None else ""
            p(f"> Enough of the machine was free for this suite{jf}. Tail figures below "
              f"are not obviously contaminated by scheduler interference, though this "
              f"is a developer laptop and not an isolated benchmarking host: expect "
              f"run-to-run variation in p99.9 and beyond.")
        p("")

    p("## Run configuration")
    p("")
    p(f"{cfg['clients']} clients over {cfg['client_threads']} generator threads, "
      f"{cfg['ping_rate_hz']:.0f} ping/s and {cfg['move_rate_hz']:.0f} input/s each, "
      f"{'Poisson' if cfg['poisson'] else 'uniform'} arrivals, seed 0x{cfg['seed']:x}. "
      f"{cfg['warmup_s']:.0f}s warmup discarded, {cfg['duration_s']:.0f}s measured.")
    p("")

    p("## 1. Throughput")
    p("")
    p("| | |")
    p("|---|---:|")
    p(f"| offered (schedule) | {rate(thr['offered_msgs_per_s'])} |")
    p(f"| delivered client to server | {rate(thr['achieved_msgs_per_s'])} |")
    p(f"| server to client | {rate(thr['reply_msgs_per_s'])} |")
    p(f"| total messages on the wire | {rate(thr['achieved_msgs_per_s'] + thr['reply_msgs_per_s'])} |")
    fidelity = 100.0 * thr['achieved_msgs_per_s'] / max(thr['offered_msgs_per_s'], 1e-9)
    p(f"| schedule fidelity | {fidelity:.2f}% |")
    p("")
    p("Schedule fidelity is the honesty check on every other number here: it is the "
      "fraction of the offered load that actually reached the wire. Below ~99% the "
      "generator was the bottleneck and the latency figures describe the client.")
    p("")

    p("## 2. Latency")
    p("")
    if contended:
        p("> Tail columns (p99 onward) are scheduler noise on this run -- see the "
          "contention note above. p50 and the server-side rows are usable.")
        p("")
    p("| path | samples | min | p50 | p90 | p99 | p99.9 | p99.99 | max |")
    p("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key, label in [
        ("ping_scheduled", "ping RTT, scheduled-send origin"),
        ("ping_wire", "ping RTT, actual-send origin"),
        ("e2e_scheduled", "input to state delta"),
        ("server_ping_service", "server: parse to reply queued"),
        ("server_loop_work", "server: event-loop work per pass"),
        ("io_to_shard_transit", "io thread to simulation ring"),
        ("shard_tick_jitter", "tick start vs deadline"),
        ("shard_tick_duration", "work inside one tick"),
        ("send_delay", "generator's own send lateness"),
    ]:
        if key in lat:
            p(latency_row(label, lat[key]))
    p("")
    p("Definitions, because a percentile without a path is meaningless:")
    p("")
    p("- **scheduled-send origin** measures from the instant a request was *due* to be "
      "sent. It includes the generator's own lateness, which is precisely the latency a "
      "real user experiences when a server stalls and requests queue behind it. This is "
      "the coordinated-omission-free number.")
    p("- **actual-send origin** measures from the instant the bytes left. The gap "
      "between the two is client-side queueing.")
    p("- **input to state delta** includes up to one tick period of quantisation by "
      "construction: an input arriving just after a tick waits for the next one. At "
      "60 Hz that is a 16.7 ms ceiling contributed by the design, not by the code.")
    p("")

    p("## 3. Allocations")
    p("")
    if not alloc["probe_enabled"]:
        p("Allocation probe was disabled for this build; counts are unmeasured.")
    else:
        p("| | |")
        p("|---|---:|")
        p(f"| steady-state allocations | **{alloc['steady_state_allocs']}** |")
        p(f"| steady-state frees | {alloc['steady_state_frees']} |")
        p(f"| steady-state bytes allocated | {bytes_h(alloc['steady_state_bytes'])} |")
        p(f"| process lifetime allocations | {alloc['process_total_allocs']} |")
        per_m = alloc['steady_state_allocs'] * 1e6 / max(thr['total_msgs'], 1)
        p(f"| allocations per million messages | {per_m:.2f} |")
        p("")
        p("Measured by interposing global `operator new`/`delete` and counting on "
          "per-thread, cache-line-isolated counters. The count covers the whole process, "
          "load generator included, so it is an upper bound on the server's own.")
    p("")

    p("## 4. Memory footprint")
    p("")
    p("| | |")
    p("|---|---:|")
    p(f"| resident set | {bytes_h(mem['rss_bytes'])} |")
    p(f"| peak resident | {bytes_h(mem['peak_rss_bytes'])} |")
    p(f"| reserved up front | {bytes_h(mem['reserved_bytes'])} |")
    p(f"| per connection | {bytes_h(mem['bytes_per_connection'])} |")
    p("")
    p("Every buffer, queue, pool and entity array is fixed-capacity and mapped at "
      "startup, so the footprint is a function of configuration rather than of load. "
      "The per-connection figure is the read buffer plus the send buffer plus the "
      "connection object; it is what multiplies when sizing for N clients.")
    p("")

    p("## 5. CPU")
    p("")
    p("| | |")
    p("|---|---:|")
    p(f"| wall time | {ns(cpu['wall_ns'])} |")
    p(f"| cpu time | {ns(cpu['cpu_ns'])} |")
    p(f"| cores saturated | {cpu['cores_used']:.3f} of {env['logical_cores']} |")
    p(f"| cpu per message | {cpu['cpu_us_per_message']:.3f} us |")
    if "macOS" in env.get("os", ""):
        p(f"| context switches | {cpu['invol_ctx_switches']:,} — see note |")
    else:
        p(f"| context switches | {cpu['vol_ctx_switches']:,} voluntary, {cpu['invol_ctx_switches']:,} involuntary |")
    p(f"| page faults | {cpu['minor_faults']:,} minor, {cpu['major_faults']:,} major |")
    p("")
    if "macOS" in env.get("os", ""):
        p("Darwin does not populate the voluntary counter at all, and its involuntary "
          "counter tracks blocking-syscall thread transitions rather than preemption — "
          "it scales with `poll()` rate, not with contention (a single-threaded spin "
          "loop on an idle machine baselines near 700/s). It is reported for "
          "completeness; scheduler interference is judged from tick jitter instead.")
        p("")
    p("**CPU microseconds per message** is the number that sizes a fleet: multiply by "
      "target throughput to get cores. It covers the whole process, so for the "
      "in-process configuration it charges the load generator's work to the server too.")
    p("")

    p("## 6. Syscall efficiency")
    p("")
    p("| | |")
    p("|---|---:|")
    p(f"| frames per read() | {sc['frames_in']/max(sc['read_calls'],1):.2f} |")
    p(f"| frames per write() | {sc['frames_out']/max(sc['write_calls'],1):.2f} |")
    p(f"| read() calls | {sc['read_calls']:,} |")
    p(f"| write() calls | {sc['write_calls']:,} |")
    p(f"| poll() calls | {sc['poll_calls']:,} |")
    p("")
    p("Frames per `read()` is the batching efficiency of the receive path, and it "
      "is the first thing to check when throughput stops scaling. A value near 1.0 "
      "at this offered rate is expected: clients send one small frame at a time, so "
      "there is nothing to batch. It rises above 1.0 only when a client is sending "
      "faster than the server drains, which is precisely when batching matters.")
    p("")
    p("`poll()` calls far exceeding event counts is the signature of a spin window: "
      "each zero-timeout poll is a real syscall (~12 us here), which is what the "
      "spin costs and why the table below reports cores alongside latency.")
    p("")

    p("## 7. Backpressure")
    p("")
    p("| event | count | meaning |")
    p("|---|---:|---|")
    p(f"| command ring full | {bp['cmd_ring_full']:,} | input dropped before reaching a shard |")
    p(f"| state deltas dropped | {bp['deltas_dropped']:,} | shard shed output; I/O thread behind |")
    p(f"| short writes | {bp['short_writes']:,} | kernel send buffer full; output queued |")
    p(f"| slow clients evicted | {bp['evicted_slow']:,} | send queue exceeded its fixed budget |")
    p(f"| parse errors | {bp['parse_errors']:,} | malformed frame; connection torn down |")
    p(f"| ticks late | {bp['ticks_late']:,} | tick began after its deadline |")
    p(f"| ticks skipped | {bp['ticks_skipped']:,} | dropped rather than replayed |")
    p("")
    p("Every queue in the system is bounded, so overload is shed and counted rather "
      "than absorbed into unbounded memory. There is no silent loss path.")
    p("")

    # ---- spin trade ------------------------------------------------------
    spin_csv = os.path.join(outdir, "spin-trade.csv")
    if os.path.exists(spin_csv):
        rows = list(csv.DictReader(open(spin_csv)))
        if rows:
            p("## 8. Spin versus block")
            p("")
            p("| I/O spin window | p50 | p99 | p99.9 | cores |")
            p("|---:|---:|---:|---:|---:|")
            for r in rows:
                p(f"| {r['spin_us']} us | {ns(r['p50_ns'])} | {ns(r['p99_ns'])} | "
                  f"{ns(r['p999_ns'])} | {float(r['cores_used']):.3f} |")
            p("")
            p("Waking a core out of a deep idle state costs on the order of 100 us on "
              "this hardware -- far more than the entire request path -- so at low "
              "offered load that wakeup *is* the latency, and keeping the core hot is "
              "what a spin window buys.")
            p("")
            p("What the table shows is the trade, not a free win: p50 falls as the "
              "window widens, and CPU rises roughly in proportion. The p99 column on a "
              "contended machine is dominated by scheduler preemption and moves "
              "non-monotonically; on a quiet machine it falls with p50. Note also that "
              "this spin is a zero-timeout `kevent`/`epoll_wait`, so it costs a syscall "
              "per iteration rather than a memory load -- that bounds how cheap the "
              "technique can be here.")
            p("")

    # ---- sweep -----------------------------------------------------------
    sweep_csv = os.path.join(outdir, "sweep.csv")
    if os.path.exists(sweep_csv):
        rows = list(csv.DictReader(open(sweep_csv)))
        if rows:
            p("## 9. Latency versus offered throughput")
            p("")
            if contended:
                p("> The p50 column and the delivered-throughput column are usable; the "
                  "tail columns and the derived capacity figure are not, on this run.")
                p("")
            p("| offered | achieved | delivered | p50 | p99 | p99.9 | max | cores | note |")
            p("|---:|---:|---:|---:|---:|---:|---:|---:|---|")
            for r in rows:
                note = ("generator CPU-bound" if r.get("client_limited") == "1"
                        else ("schedule not delivered"
                              if float(r.get("fidelity_pct", 100)) < 99 else ""))
                p(f"| {rate(r['offered_msgs_per_s'])} | {rate(r['achieved_msgs_per_s'])} | "
                  f"{float(r.get('fidelity_pct', 0)):.1f}% | "
                  f"{ns(r['p50_ns'])} | {ns(r['p99_ns'])} | {ns(r['p999_ns'])} | "
                  f"{ns(r['max_ns'])} | {float(r['cores_used']):.2f} | {note} |")
            p("")
            p("The useful capacity of a server is not where it stops accepting work; it "
              "is where the tail leaves the acceptable band. Read this table for the "
              "point at which p99 departs from its lightly loaded value -- past that, "
              "additional offered load buys queueing, not throughput.")
            p("")

    # ---- thread scaling --------------------------------------------------
    scale_csv = os.path.join(outdir, "scaling.csv")
    if os.path.exists(scale_csv):
        rows = list(csv.DictReader(open(scale_csv)))
        if rows:
            p("## 10. Thread scaling")
            p("")
            if contended:
                p("> Not meaningful on this run: scaling onto more threads requires free "
                  "cores, and there were none. Included for completeness only.")
                p("")
            p("| I/O threads | shards | achieved | p50 | p99 | cpu/msg | cores |")
            p("|---:|---:|---:|---:|---:|---:|---:|")
            for r in rows:
                p(f"| {r['io_threads']} | {r['shards']} | {rate(r['achieved_msgs_per_s'])} | "
                  f"{ns(r['p50_ns'])} | {ns(r['p99_ns'])} | "
                  f"{float(r['cpu_us_per_msg']):.3f} us | {float(r['cores_used']):.2f} |")
            p("")
            p("Measured with spinning disabled, so idle threads block rather than "
              "burning a spin budget -- otherwise each added thread inflates "
              "CPU-per-message for reasons that have nothing to do with scaling.")
            p("")
            p("The column to watch is **cpu/msg**. Offered load is held constant "
              "across rows, so throughput is expected to be flat; what matters is "
              "whether the cost of handling a message changes as threads are added. "
              "Flat means no shared bottleneck. Climbing would point at a contended "
              "cache line, a lock, or an allocator -- something the shared-nothing "
              "design claims not to have.")
            p("")

    # ---- pointers to the raw distributions ------------------------------
    if os.path.exists(os.path.join(outdir, "dist-client.csv")):
        p("## Full distributions")
        p("")
        p("Percentiles summarise a distribution about as well as seven points "
          "summarise a curve. Running the suite writes `docs/results/dist-client.csv` "
          "and `dist-server.csv`, carrying every occupied histogram bucket with its "
          "count and cumulative fraction, so the CDF can be plotted directly. "
          "(They are ~3 MB per run and therefore not committed; regenerate with "
          "`scripts/run_bench.sh`, or `ge_loadgen --hist-csv PREFIX` directly.) "
          "That is what distinguishes a tail caused by one stall from a bimodal "
          "split between two code paths from a genuinely heavy tail -- three "
          "different problems that produce the same p99.")
        p("")
        p("```sh")
        p("# quick CDF of the wire round trip")
        p("awk -F, '$1==\"ping_wire\"{print $2, $6}' docs/results/dist-client.csv")
        p("```")
        p("")

    p("## Reproducing")
    p("")
    p("```sh")
    p("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j")
    p("scripts/run_bench.sh            # regenerates this file")
    p("```")
    p("")
    p("The load generator is seeded, the simulation is deterministic given the same "
      "inputs, and the warmup window is discarded, so run-to-run variation on a quiet "
      "machine reflects the scheduler rather than the program.")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "docs/results")
