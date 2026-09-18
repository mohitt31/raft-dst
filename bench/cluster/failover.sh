#!/usr/bin/env bash
# Kills the leader while a client writes, and reports how long the cluster
# was unavailable from the client's point of view. Repeats N times.
set -euo pipefail
cd "$(dirname "$0")/../.."

PEERS=${1:-127.0.0.1:7401,127.0.0.1:7402,127.0.0.1:7403}
DATA=${2:-/var/tmp/raft-dst-bench}
KILLS=${RAFTDST_KILLS:-20}
RATE=${RAFTDST_FAILOVER_RATE:-500}

IFS=',' read -ra ADDRS <<< "$PEERS"

leader_port() {
  # The leader is the node that answers a get without a NotLeader hint.
  for a in "${ADDRS[@]}"; do
    port=${a##*:}
    if build/release/tools/raft_client --port "$port" get __probe__ >/dev/null 2>&1; then
      echo "$port"; return 0
    fi
  done
  return 1
}

echo "kill,gap_ms"
for k in $(seq 1 "$KILLS"); do
  port=$(leader_port) || { echo "no leader; aborting"; exit 1; }
  id=0
  for i in "${!ADDRS[@]}"; do [[ "${ADDRS[$i]##*:}" == "$port" ]] && id=$i; done

  # Start writing, kill the leader one second in, and read the gap the
  # client saw between successful commits.
  ( sleep 1; pkill -f "raftd --id $id " || true ) &
  gap=$(build/release/bench/cluster/load_gen --peers "$PEERS" --rate "$RATE" --seconds 8 \
          --warmup 0 --conns 16 | awk '/longest_gap_between_commits_ms/ {print $2}')
  echo "$k,$gap"

  # Bring it back for the next round.
  build/release/tools/raftd --id "$id" --peers "$PEERS" --wal "$DATA/n$id.wal" \
    > "$DATA/n$id.log" 2>&1 &
  sleep 3
done
