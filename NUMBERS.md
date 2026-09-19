# Numbers

Every performance number in this repository is listed here with the exact
command that reproduces it, the machine and the build flags. If a number is
not in this file, it has not been measured and must not appear anywhere else.

`bench/reproduce.sh` regenerates all of them.

## Machines

| Name | Hardware | OS | Compiler |
|---|---|---|---|
| mac | Apple M4, 10 cores, 128-byte cache lines, 16 KB pages | macOS 26.5 | Apple Clang 21.0.0 |
| linux | bare-metal x86-64 (details recorded when first used) | — | — |

Release flags: `-O2 -g -DNDEBUG`. Sanitizers off. Internal DCHECKs off in
release; the safety checker is **on** in every measurement below, because
that is how the fuzzer actually runs.

## Simulator throughput

mac, release, measured 2026-09-18:

    cmake --preset release && cmake --build --preset release
    build/release/bench/sim_bench --seeds 100 --repeats 5 --nodes 5

| Metric | Value |
|---|---|
| simulated cluster-hours per wall-clock minute | 13.6 |
| injected faults per wall-clock second | 382,244 |
| simulator events per wall-clock second | 3,620,273 |

Median of 5 repetitions after a warmup pass. Each repetition does identical
work: 100 seeds, 5 nodes, the fuzzer's chaos profile, 107,415,060 events,
11,341,352 injected faults, 921,507 entries committed, and the same trace
fingerprint `7dcb3c30127ee56a` every time. Two separate runs on a quiet
machine gave 13.6 both times; a run alongside other load gave 14.5, so treat
±1 as the noise floor.

One simulated cluster-hour costs about 4.4 wall seconds here, and the fault
rate is roughly six orders of magnitude above what a real cluster would see.

## Fuzz campaign on the clean engine

mac, release, measured 2026-09-18:

    build/release/tools/raft_fuzz --seeds 1:3000 --keep-going

| Metric | Value |
|---|---|
| seeds | 2,999 |
| failures | 0 |
| entries committed | 23,218,658 |
| client writes acknowledged | 23,193,971 |
| faults injected | 171,478,477 |

An earlier campaign over seeds 300–3000 found one failure, seed 2974, which
turned out to be a false positive in the checker rather than a defect in the
engine (DESIGN.md section 11a). The case is kept in
`tests/regressions/checker-stale-leader-term.repro`.

## Fuzzer effectiveness

How many randomized seeds the fuzzer needs to catch each deliberately
planted bug. Build: `mutants` preset (release flags, planted bugs on,
internal DCHECKs off so the external checker is what gets measured), mac,
measured 2026-09-18 after the checker fix:

    cmake --preset mutants && cmake --build --preset mutants
    for m in 1 2 3 4 5 6; do build/mutants/tools/raft_fuzz --mutation $m --seeds 1:2000; done

| # | Planted bug | Seeds to detect | Reported as |
|---|---|---|---|
| 1 | commit earlier-term entries by counting (Figure 8) | 194 | leader of term 39 is missing committed entry 552 |
| 2 | vote without the up-to-date log check | 1 | leader of term 13 is missing committed entry 147 |
| 3 | a second vote in one term | 2 | vote in term 1 changed from n1 to n4 (after sync) |
| 4 | truncate at prev_index without a conflict | 1 | leader of term 37 is missing committed entry 1147 |
| 5 | follower commit not capped at verified entries | 1,561 | committed a different entry at index 7681 |
| 6 | persist the term but not the vote | 1 | vote in term 1 changed from n1 to none (on restart) |

Bugs 1 and 5 are also caught by targeted scenarios in
`tests/sim/mutants_test.cc`, in well under a millisecond each, which is why
those scenarios exist alongside the random search.

## Cross-platform determinism

The same seeds must produce the same trace everywhere. Measured 2026-09-19:

    build/release/tools/raft_fuzz --seeds 1:30 --fingerprints

| Platform | Compiler | Standard library | Result |
|---|---|---|---|
| macOS 26.5, arm64 | Apple Clang 21.0.0 | libc++ | 29 fingerprints (the range is half-open) |
| Ubuntu 24.04, aarch64 (container) | GCC 13.3.0 | libstdc++ | identical to the above |

Both are arm64, so this shows independence from the OS, compiler and
standard library. A cross-architecture comparison (arm64 vs x86-64) runs in
CI and can be done by hand by comparing the fingerprints `bench/reproduce.sh`
prints on two machines.

All 123 tests also pass under GCC 13 on Ubuntu 24.04.

## Real cluster

Not measured yet. Commit latency (p50/p99/p99.9 under fixed-rate load) and
failover time need the Linux box; `bench/reproduce.sh` skips that section
until `RAFTDST_CLUSTER_HOSTS` is set.
