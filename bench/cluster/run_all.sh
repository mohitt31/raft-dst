#!/usr/bin/env bash
# Cluster benchmarks. Run on the Linux box.
#
#   RAFTDST_CLUSTER_HOSTS=local bench/cluster/run_all.sh
#
# "local" runs three raftd processes on this machine, each pinned to its own
# core: that is a single-host loopback measurement, not a network one, and
# NUMBERS.md labels it that way. Set RAFTDST_CLUSTER_HOSTS to a comma
# separated host:port list to measure three real machines instead.
set -euo pipefail
cd "$(dirname "$0")/../.."

RATE=${RAFTDST_RATE:-2000}
SECONDS_TO_RUN=${RAFTDST_SECONDS:-60}
WARMUP=${RAFTDST_WARMUP:-10}
CONNS=${RAFTDST_CONNS:-32}
DATA=${RAFTDST_DATA:-/var/tmp/raft-dst-bench}
PEERS=${RAFTDST_CLUSTER_HOSTS:-local}

cmake --preset release >/dev/null
cmake --build --preset release >/dev/null

if [[ "$PEERS" == "local" ]]; then
  PEERS=127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403
  rm -rf "$DATA"; mkdir -p "$DATA"
  pins=(${RAFTDST_PINS:-2 3 4})
  for i in 0 1 2; do
    cmd=(build/release/tools/raftd --id "$i" --peers "$PEERS" --wal "$DATA/n$i.wal")
    if command -v taskset >/dev/null; then
      taskset -c "${pins[$i]}" "${cmd[@]}" > "$DATA/n$i.log" 2>&1 &
    else
      "${cmd[@]}" > "$DATA/n$i.log" 2>&1 &
    fi
  done
  started_local=1
  sleep 2
fi

echo "=== machine ==="
uname -srm
if [[ -r /proc/cpuinfo ]]; then grep -m1 'model name' /proc/cpuinfo; fi
if command -v lsblk >/dev/null; then lsblk -no NAME,MODEL,ROTA "$(df --output=source "$DATA" | tail -1 | sed 's/[0-9]*$//')" 2>/dev/null || true; fi
echo "peers: $PEERS"
echo

echo "=== commit latency at ${RATE}/s for ${SECONDS_TO_RUN}s ==="
build/release/bench/cluster/load_gen --peers "$PEERS" --rate "$RATE" \
  --seconds "$SECONDS_TO_RUN" --warmup "$WARMUP" --conns "$CONNS"
echo

echo "=== failover: kill the leader under load ==="
bench/cluster/failover.sh "$PEERS" "$DATA"

if [[ "${started_local:-0}" == "1" ]]; then
  pkill -f 'raftd --id' || true
fi
