# Linux x86-64 results

Build, test and measurement results for the first x86-64 run of this
repository, measured 2026-09-25. Every number here was measured on the
machine described below with the exact command shown. Nothing was copied
from `NUMBERS.md`; the Mac numbers are quoted only where this file compares
against them, and each such quote is labelled "mac".

No engine, test or tool code was changed for any of this.

## Machine

This is **not** the laptop the run was planned for (a Lenovo LOQ with a
hybrid Intel i5 under WSL2). It is a cloud virtual machine. It is x86-64,
so the cross-architecture determinism check is valid, but its throughput
and latency numbers describe this VM only.

| Item | Value |
|---|---|
| Name in this file | `x86vm` |
| Kind | KVM guest in a cloud sandbox (`lscpu`: Hypervisor vendor KVM, full virtualization; `systemd-detect-virt`: docker). Not WSL2, not bare metal. |
| CPU | Intel(R) Xeon(R) Processor @ 2.10GHz, family 6 model 207 (reported to the guest as-is), AVX-512, 4 vCPUs |
| Topology seen by the guest | 1 socket, 4 cores, 1 thread per core, CPUs 0-3, each with its own L1/L2, one shared 260 MiB L3. No hybrid cores: all four vCPUs are identical, so there are no P-cores or E-cores to choose between. Whether the vCPUs are hyperthreads of the same host core is not visible from inside the guest. |
| CPUs used | Step 4 (simulator): CPU 1 via `taskset -c 1`. Step 5 (cluster): nodes on CPUs 1, 2, 3; `load_gen` and `raft_client` unpinned. CPU 0 left to the OS. |
| RAM | 15 GiB, no swap (`free -h`) |
| Disk | `/dev/vda`, virtio block device, ext4, 252 GB. Reported rotational (`ROTA 1`) as virtio devices usually are; the real backing storage is not visible. The cluster write-ahead logs live on this disk. |
| OS / kernel | Ubuntu 24.04.4 LTS, `Linux 6.18.44-fc-v37 #1 SMP PREEMPT_DYNAMIC x86_64` |
| Compiler | g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, libstdc++ |
| Tools | cmake 3.28.3, ninja 1.11.1, git 2.43.0 |
| `vm.mmap_rnd_bits` | 28 already; no sysctl change was needed |
| Power | Not applicable: a VM with no `/sys/class/power_supply` entries. AC/battery was not measured. |
| CPU frequency control | Not visible (no `cpufreq` in the guest). Other tenants on the host may share the physical cores. |
| Commit tested | `2723355 Build and test on Linux with gcc, and compare traces across platforms` |

Build flags are the repository presets, unchanged. Release and mutants:
`-O2 -g -DNDEBUG`. The safety checker is on in every simulator measurement.

## Findings

1. **All tests pass in every configuration**, with the same test counts
   as the Mac: release 123/123, asan 124/124, tsan 124/124, mutants
   125/125. ThreadSanitizer ran with no mapping error.
2. **Cross-architecture determinism holds: 29 seeds identical across arm64
   and x86-64.** `raft_fuzz --seeds 1:30 --fingerprints` on this machine is
   byte-identical to the list from the arm64 Mac (`diff` prints nothing).
   Every other seed-only result also matches the Mac exactly: the per-pass
   work and fingerprint of `sim_bench` (`7dcb3c30127ee56a`), the 3000-seed
   campaign totals, and the seed count and message for all six planted
   bugs.
3. **The 3000-seed campaign on the clean engine: 2,999 seeds, 0 failures.**
4. **Bug in `bench/cluster/failover.sh`: it cannot find the leader and
   aborts on the first kill** (`no leader; aborting`, exit 1). This is a
   bug in the benchmark script, not in the engine. Details and root cause
   are in "Failover time" below. The failover numbers in this file come
   from a workaround that changes no code: the `__probe__` key is written
   once before the unmodified script runs.
5. Because of finding 4, `bench/cluster/run_all.sh` exits 1 after the
   latency section on every run, and it leaves the three `raftd` processes
   running: `set -e` stops the script before its final `pkill`. I stopped
   them by hand after each run.
6. Cluster tail latency on this VM is not repeatable from run to run
   (details under "Cluster commit latency"). Only p50 is stable enough to
   quote.

## Tests

    for p in release asan tsan mutants; do
      cmake --preset $p && cmake --build --preset $p && ctest --preset $p
    done

| Preset | Result | ctest wall time |
|---|---|---|
| release | 123/123 passed | 6.38 s |
| asan (ASan + UBSan) | 124/124 passed | 42.80 s |
| tsan | 124/124 passed | 54.22 s |
| mutants | 125/125 passed | 43.81 s |

GoogleTest was fetched by CMake on first configure. `sim.cross_process_determinism`
passed in all four configurations.

## Cross-architecture determinism

    build/release/tools/raft_fuzz --seeds 1:30 --fingerprints > fp-x86.txt
    diff fp-expected.txt fp-x86.txt

`fp-expected.txt` is the list from the arm64 Mac (macOS 26.5, Apple Clang
21.0.0, libc++), which was also confirmed on arm64 Linux with GCC 13.
Result: **identical, 29 seeds** (the range is half-open). Since no seed
differed, no trace file was needed.

| Platform | Compiler | Standard library | Result |
|---|---|---|---|
| macOS 26.5, arm64 (mac) | Apple Clang 21.0.0 | libc++ | reference |
| Ubuntu 24.04, aarch64 container (mac, earlier) | GCC 13.3.0 | libstdc++ | identical |
| Ubuntu 24.04, x86-64 KVM VM (`x86vm`, this file) | GCC 13.3.0 | libstdc++ | **identical** |

This is the first comparison across architectures. `reproduce.sh` printed
the same fingerprints again for seeds 1-19.

## Simulator throughput

x86vm, release, pinned to CPU 1:

    taskset -c 1 bash bench/reproduce.sh              # first run, inside the script
    taskset -c 1 build/release/bench/sim_bench --seeds 100 --repeats 5 --nodes 5   # second run

| Metric | Run 1 (reproduce.sh) | Run 2 | Difference |
|---|---|---|---|
| simulated cluster-hours per wall-clock minute | 5.1 | 5.2 | |
| injected faults per wall-clock second | 143,719 | 144,549 | 0.6% |
| simulator events per wall-clock second | 1,361,180 | 1,369,035 | 0.6% |
| wall seconds per pass (median of 5) | 79.57 | 77.96 | 2.0% |

The two runs are within 5% of each other, so this measurement shows no
sign of thermal throttling or noisy neighbours.

Each pass did identical work in both runs, and it matches the work the Mac
reported in `NUMBERS.md`: 107,415,060 events, 11,341,352 faults, 921,507
entries committed, 920,680 acknowledged, 0 violations, fingerprint
`7dcb3c30127ee56a`.

Caveat: one vCPU of a shared cloud host at 2.10 GHz. This is not the
laptop's P-core and not bare metal.

## Fuzz campaign on the clean engine

x86vm, release, pinned to CPU 1, run by `reproduce.sh`:

    build/release/tools/raft_fuzz --seeds 1:3000 --keep-going

| Metric | Value |
|---|---|
| seeds | 2,999 |
| failures | 0 |
| entries committed | 23,218,658 |
| client writes acknowledged | 23,193,971 |
| faults injected | 171,478,477 |

All five values are equal to the mac campaign in `NUMBERS.md`, as they
should be if determinism holds, because they depend only on the seeds.

The whole `reproduce.sh` run (build check, sim_bench, six mutation searches,
campaign, fingerprints) took 2,845 s of wall time on CPU 1.

## Fuzzer effectiveness

x86vm, `mutants` preset, pinned to CPU 1, run by `reproduce.sh`:

    for m in 1 2 3 4 5 6; do build/mutants/tools/raft_fuzz --mutation $m --seeds 1:2000; done

| # | Seeds to detect | Reported as | Same as mac |
|---|---|---|---|
| 1 | 194 | leader of term 39 is missing committed entry 552 | yes |
| 2 | 1 | leader of term 13 is missing committed entry 147 | yes |
| 3 | 2 | vote in term 1 changed from n1 to n4 (after sync) | yes |
| 4 | 1 | leader of term 37 is missing committed entry 1147 | yes |
| 5 | 1,561 | committed a different entry at index 7681 (term 109, first committed with term 88) | yes |
| 6 | 1 | vote in term 1 changed from n1 to none (on restart) | yes |

## Cluster commit latency

**Environment label for every cluster number: KVM cloud VM, virtio disk,
single-host loopback: 3 processes, 1 machine, 1 shared disk. Not bare
metal, and not WSL2.** Each WAL write's fsync goes to a virtual disk, so
commit latency here is not what native Linux on real hardware would show.
Booting a real machine into Linux (for example an Ubuntu live USB) would
give bare-metal numbers.

    RAFTDST_DATA=$PWD/build/cluster-data RAFTDST_CLUSTER_HOSTS=local \
      RAFTDST_PINS="1 2 3" RAFTDST_RATE=<rate> bench/cluster/run_all.sh

Defaults from the script: 60 s measured after 10 s warmup, 32 connections,
32-byte values. `RAFTDST_DATA` was pointed inside `build/` because the
script runs `rm -rf` on that directory.

`commit_latency_us` is measured from each request's due time, so it
includes queueing. `service_time_us` is measured from the actual send.

| Rate/s | Run | Saturated? | Achieved/s | commit p50 µs | p99 µs | p99.9 µs | max µs | service p50 µs | service p99 µs |
|---|---|---|---|---|---|---|---|---|---|
| 500 | 1 | no | 500 | 1,216 | 10,870 | 100,942 | 145,348 | 1,092 | 9,552 |
| 1000 | 1 | no | 1000 | 764 | 2,989 | 13,701 | 110,441 | 668 | 2,767 |
| 1000 | 2 | no | 1000 | 1,112 | 207,194 | 467,232 | 493,923 | 964 | 23,144 |
| 2000 | 1 | no | 2000 | 699 | 87,856 | 226,800 | 253,951 | 605 | 19,193 |
| 2000 | 2 | no | 2000 | 3,283 | 700,892 | 727,642 | 734,919 | 3,137 | 33,460 |
| 3000 | 1 | **SATURATED** | 2767 | — | — | — | — | 9,375 | 33,030 |
| 4000 | 1 | **SATURATED** | 3254 | — | — | — | — | 8,634 | 25,357 |

Latencies of saturated runs are left out on purpose; the raw logs below
keep them.

**Largest rate that did not saturate: 2000/s** (both runs), by `load_gen`'s
own rule (`SATURATED` is printed when commit p50 > 2 × service p50 + 1 ms).
3000/s and 4000/s saturate.

Caveats:

* **Tails are not repeatable here.** p99 at 1000/s was 3.0 ms in one run
  and 207 ms in the next; at 2000/s it was 88 ms and then 701 ms. In the
  bad runs the commit p99 is far above the service p99, so requests were
  queueing for stretches of the run even though p50 was fine. The
  saturation rule only looks at p50, so it does not catch this. The likely
  causes are fsync stalls on the virtual disk and noisy neighbours on the
  shared host; neither was measured directly. Only p50 should be quoted
  from this machine, and even that moved from 699 µs to 3,283 µs between
  the two 2000/s runs.
* Every run had `not_leader 22`. These requests hit a follower first and
  were redirected; none were lost (`lost 0 errors 0`).
* `load_gen` shares the 4 vCPUs with the nodes: it runs unpinned and most
  likely lands on CPU 0.

## Failover time

Same environment label as above.

**`bench/cluster/failover.sh` does not work as committed.** It finds the
leader by running `raft_client --port P get __probe__` against each port and
taking the first one that exits 0. The key `__probe__` is never written, so
the leader answers `kNotFound` and `raft_client` exits 1
(`tools/raft_client.cc`, the `kNotFound` case). Followers also exit 1, after
"no leader answered". No port ever exits 0, `leader_port` fails, and the
script prints `no leader; aborting`. I confirmed this by hand on the running
cluster: port 7403 (the leader) printed `(not found)` with exit 1, and ports
7401 and 7402 printed `no leader answered` with exit 1. After `put
__probe__ 1`, a `get` exits 0 on 7403 and 1 on the other two. That is the
behaviour the script expects. A fix would be to write the probe key first,
or to treat `(not found)` as "this node is the leader". I did not make
either change.

Workaround used for the numbers below (no code changed): start the cluster
with the same commands `run_all.sh` uses, write the key once, then run the
unmodified script.

    DATA=$PWD/build/cluster-data; PEERS=127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403
    rm -rf "$DATA"; mkdir -p "$DATA"; pins=(1 2 3)
    for i in 0 1 2; do
      taskset -c ${pins[$i]} build/release/tools/raftd --id $i --peers $PEERS \
        --wal $DATA/n$i.wal > $DATA/n$i.log 2>&1 &
    done; sleep 2
    build/release/tools/raft_client --port 7401 --port 7402 --port 7403 put __probe__ 1
    RAFTDST_KILLS=20 bench/cluster/failover.sh "$PEERS" "$DATA"

The script writes at 500/s with 16 connections, kills the leader one second
in, and reports the longest gap between successful commits. The restarted
node is not pinned (that is how the script restarts it).

| Statistic (N = 20 kills) | Gap ms |
|---|---|
| min | 161.7 |
| p50 (nearest rank, 10th of 20) | 228.8 |
| p99 (nearest rank, 20th of 20; with N = 20 this is the maximum) | 291.8 |
| max | 291.8 |

p99.9 is not reported: 20 samples cannot support it.

## Not measured

* Anything on the Lenovo LOQ laptop: WSL2 behaviour, P-core vs E-core,
  AC vs battery. This session ran on a cloud VM instead.
* Bare-metal cluster latency (see the label above).
* Failover with the committed `failover.sh` as is: it aborts (finding 4).

## Raw output

### ctest summary lines

```
release:  100% tests passed, 0 tests failed out of 123   Total Test time (real) =   6.38 sec
asan:     100% tests passed, 0 tests failed out of 124   Total Test time (real) =  42.80 sec
tsan:     100% tests passed, 0 tests failed out of 124   Total Test time (real) =  54.22 sec
mutants:  100% tests passed, 0 tests failed out of 125   Total Test time (real) =  43.81 sec
```

### Fingerprints (x86vm) and diff against the arm64 list

```
$ build/release/tools/raft_fuzz --seeds 1:30 --fingerprints
1 d0e502821b1de264
2 9bfe096c9d6a8c57
3 96a6ac0fa8ab8ea3
4 f79a5962a648e46f
5 581b13bd34f82494
6 01863f6f97c887d8
7 8bbd206f5f06d299
8 b8022438c454a584
9 97083079ac21b65b
10 dab219b56fd257d5
11 ae1c9f9ab0706b8c
12 b76a1d0ced50e9e7
13 1df176b248eb72ec
14 cf2dda1e84e0888b
15 c766339efc688f6c
16 487298177b51d5fb
17 bc2a5e8ead0f4f8c
18 6e4398a1dd8af5f1
19 79f5cc2c14e0d820
20 4c303dca99516df6
21 f7414f3d3c193449
22 47d7712ac98b909d
23 e60f7456c2c12bc7
24 8b02e3c055b00797
25 661377c72f21a537
26 c598df5adffbc989
27 20a5065157b20103
28 1512928af45d17bc
29 ada83bc3bad9dc0b
$ diff fp-expected.txt fp-x86.txt && echo IDENTICAL
IDENTICAL
```

### `taskset -c 1 bash bench/reproduce.sh` (reproduce-x86.log)

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0

=== build (release, mutants) ===
release flags: -O2 -g -DNDEBUG

=== simulator throughput (nodes=5, chaos profile, checker on) ===
seeds=100 nodes=5 repeats=5
simulated_cluster_hours_per_wall_minute 5.1
injected_faults_per_wall_second 143719
simulator_events_per_wall_second 1361180
per_pass: wall_seconds=79.57 events=107415060 faults=11341352 committed=921507 acked=920680 violations=0 fingerprint=7dcb3c30127ee56a

=== fuzzer effectiveness: seeds to catch each planted bug ===
mutation 1 detected after 194 seeds (seed 194): t=14360337353 n0: leader of term 39 is missing committed entry 552
mutation 2 detected after 1 seeds (seed 1): t=4452982265 n2: leader of term 13 is missing committed entry 147
mutation 3 detected after 2 seeds (seed 2): t=80922080 n1: vote in term 1 changed from n1 to n4 (after sync)
mutation 4 detected after 1 seeds (seed 1): t=14750399012 n1: leader of term 37 is missing committed entry 1147
mutation 5 detected after 1561 seeds (seed 1561): t=20943426563 n1: committed a different entry at index 7681 (term 109, first committed with term 88)
mutation 6 detected after 1 seeds (seed 1): t=3958443276 n1: vote in term 1 changed from n1 to none (on restart)

=== fuzz campaign on the clean engine ===
2999 seeds, 0 failures, 23218658 entries committed, 23193971 writes acknowledged, 171478477 faults injected

=== determinism fingerprints (compare these across machines) ===
1 d0e502821b1de264
2 9bfe096c9d6a8c57
3 96a6ac0fa8ab8ea3
4 f79a5962a648e46f
5 581b13bd34f82494
6 01863f6f97c887d8
7 8bbd206f5f06d299
8 b8022438c454a584
9 97083079ac21b65b
10 dab219b56fd257d5
11 ae1c9f9ab0706b8c
12 b76a1d0ced50e9e7
13 1df176b248eb72ec
14 cf2dda1e84e0888b
15 c766339efc688f6c
16 487298177b51d5fb
17 bc2a5e8ead0f4f8c
18 6e4398a1dd8af5f1
19 79f5cc2c14e0d820

=== real cluster ===
skipped: set RAFTDST_CLUSTER_HOSTS to run the Linux cluster benchmarks
(wall time of the whole command: 2845 s, 19:36:00 to 20:23:25 UTC)
```

### `taskset -c 1 build/release/bench/sim_bench --seeds 100 --repeats 5 --nodes 5` (second run)

```
seeds=100 nodes=5 repeats=5
simulated_cluster_hours_per_wall_minute 5.2
injected_faults_per_wall_second 144549
simulator_events_per_wall_second 1369035
per_pass: wall_seconds=77.96 events=107415060 faults=11341352 committed=921507 acked=920680 violations=0 fingerprint=7dcb3c30127ee56a
```

### cluster-500.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 500/s for 60s ===
offered_rate_per_s 500
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 30000
committed 30000 not_leader 22 lost 0 errors 0
achieved_rate_per_s 500 (over 60.0 s of successes)
commit_latency_us p50 1216 p99 10870 p99.9 100942 max 145348
service_time_us p50 1092 p99 9552 p99.9 94730
longest_gap_between_commits_ms 135.3

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-1000.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 1000/s for 60s ===
offered_rate_per_s 1000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 60000
committed 60000 not_leader 22 lost 0 errors 0
achieved_rate_per_s 1000 (over 60.0 s of successes)
commit_latency_us p50 764 p99 2989 p99.9 13701 max 110441
service_time_us p50 668 p99 2767 p99.9 12700
longest_gap_between_commits_ms 27.8

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-1000-run2.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 1000/s for 60s ===
offered_rate_per_s 1000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 60001
committed 60001 not_leader 22 lost 0 errors 0
achieved_rate_per_s 1000 (over 60.0 s of successes)
commit_latency_us p50 1112 p99 207194 p99.9 467232 max 493923
service_time_us p50 964 p99 23144 p99.9 114306
longest_gap_between_commits_ms 244.7

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-2000.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 2000/s for 60s ===
offered_rate_per_s 2000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 120011
committed 120011 not_leader 22 lost 0 errors 0
achieved_rate_per_s 2000 (over 60.0 s of successes)
commit_latency_us p50 699 p99 87856 p99.9 226800 max 253951
service_time_us p50 605 p99 19193 p99.9 52862
longest_gap_between_commits_ms 100.9

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-2000-run2.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 2000/s for 60s ===
offered_rate_per_s 2000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 120010
committed 120010 not_leader 22 lost 0 errors 0
achieved_rate_per_s 2000 (over 60.0 s of successes)
commit_latency_us p50 3283 p99 700892 p99.9 727642 max 734919
service_time_us p50 3137 p99 33460 p99.9 49387
longest_gap_between_commits_ms 52.8

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-3000.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 3000/s for 60s ===
offered_rate_per_s 3000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 180106
committed 180106 not_leader 22 lost 0 errors 0
achieved_rate_per_s 2767 (over 65.1 s of successes)
commit_latency_us p50 3546477 p99 5461714 p99.9 5529630 max 5534273
service_time_us p50 9375 p99 33030 p99.9 49013
longest_gap_between_commits_ms 81.4
SATURATED: latency is dominated by queueing, so the offered rate of 3000/s is above what this cluster commits; lower --rate before quoting these latencies

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### cluster-4000.log

```
=== machine ===
Linux 6.18.44-fc-v37 x86_64
model name	: Intel(R) Xeon(R) Processor @ 2.10GHz
vda           1
peers: 127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403

=== commit latency at 4000/s for 60s ===
offered_rate_per_s 4000
measured_seconds 60.0 connections 32 value_bytes 32
requests_after_warmup 241167
committed 241167 not_leader 22 lost 0 errors 0
achieved_rate_per_s 3254 (over 74.1 s of successes)
commit_latency_us p50 4097461 p99 14031675 p99.9 14096319 max 14112974
service_time_us p50 8634 p99 25357 p99.9 40154
longest_gap_between_commits_ms 64.3
SATURATED: latency is dominated by queueing, so the offered rate of 4000/s is above what this cluster commits; lower --rate before quoting these latencies

=== failover: kill the leader under load ===
kill,gap_ms
no leader; aborting
```

### Failover with the probe key written first (cluster-failover.log)

```
probe put: ok
get on 7401 exit=1
get on 7402 exit=1
get on 7403 exit=0
kill,gap_ms
1,280.3
2,257.8
3,230.2
4,255.8
5,291.8
6,226.7
7,228.1
8,170.0
9,258.8
10,200.9
11,290.4
12,164.0
13,228.8
14,254.6
15,184.7
16,161.7
17,264.6
18,199.9
19,196.7
20,247.6
failover exit=0
```

### Leader probe by hand, on the cluster left running after cluster-500.log

```
port 7401: no leader answered   exit=1
port 7402: no leader answered   exit=1
port 7403: (not found)          exit=1
```
