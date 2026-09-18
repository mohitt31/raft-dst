Cluster benchmarks. These need the Linux box; nothing here runs as part of
`ctest`.

    RAFTDST_CLUSTER_HOSTS=local bench/cluster/run_all.sh

`local` starts three `raftd` processes on this machine, each pinned to its
own core with `taskset`. Those are **single-host loopback** numbers: no
switch, no NIC, and the three write-ahead logs share one disk. NUMBERS.md
labels them that way. To measure three real machines, set
`RAFTDST_CLUSTER_HOSTS` to their `host:port` list and start `raftd` on each.

Knobs: `RAFTDST_RATE`, `RAFTDST_SECONDS`, `RAFTDST_WARMUP`, `RAFTDST_CONNS`,
`RAFTDST_KILLS`, `RAFTDST_PINS`, `RAFTDST_DATA`.

`load_gen` reports two latencies:

* `commit_latency_us` measured from when each request was **due** under the
  fixed rate, which includes queueing. This is the number to quote, because
  it does not hide the tail behind a client that slowed down with the server.
* `service_time_us` measured from when the request was actually sent.

If the first is far above the second, the offered rate was above what the
cluster can commit and the run printed `SATURATED`. Lower `--rate` until it
stops, then quote `commit_latency_us`.

`failover.sh` writes at a steady rate, kills the leader one second in, and
reports the longest gap between successful commits: the client-visible
unavailability window. It repeats `RAFTDST_KILLS` times, one line per kill,
so the output is a sample set, not a single number.
