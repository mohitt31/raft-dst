#!/usr/bin/env bash
# Regenerates every number in README.md and NUMBERS.md.
#
#   bench/reproduce.sh              simulator numbers (any machine)
#   bench/reproduce.sh --quick      fewer seeds, for a smoke check
#
# The real-cluster section needs the Linux box and is skipped unless
# RAFTDST_CLUSTER_HOSTS is set (see bench/cluster/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

SEEDS=100
REPEATS=5
MUTATION_SEEDS=2000
QUICK=0
if [[ "${1:-}" == "--quick" ]]; then
  QUICK=1; SEEDS=20; REPEATS=3; MUTATION_SEEDS=200
fi

if [[ $QUICK -eq 1 ]]; then
  echo "QUICK MODE: a smoke check, not the published numbers."
  echo "  Throughput uses a different seed set, so it is NOT comparable to"
  echo "  NUMBERS.md, and 200 seeds is too few to catch planted bugs 1 and 5."
  echo
fi

echo "=== machine ==="
uname -srm
if command -v sw_vers >/dev/null; then sw_vers | tr '\n' ' '; echo; fi
c++ --version | head -1
echo

echo "=== build (release, mutants) ==="
cmake --preset release >/dev/null
cmake --build --preset release >/dev/null
cmake --preset mutants >/dev/null
cmake --build --preset mutants >/dev/null
echo "release flags: $(grep -A1 '"name": "release"' CMakePresets.json | grep -o '\-O2[^"]*' || echo '-O2 -g -DNDEBUG')"
echo

echo "=== simulator throughput (nodes=5, chaos profile, checker on) ==="
build/release/bench/sim_bench --seeds "$SEEDS" --repeats "$REPEATS" --nodes 5
echo

echo "=== fuzzer effectiveness: seeds to catch each planted bug ==="
for m in 1 2 3 4 5 6; do
  build/mutants/tools/raft_fuzz --mutation "$m" --seeds "1:$MUTATION_SEEDS" || true
done
echo

echo "=== real cluster ==="
if [[ -n "${RAFTDST_CLUSTER_HOSTS:-}" ]]; then
  bench/cluster/run_all.sh
else
  echo "skipped: set RAFTDST_CLUSTER_HOSTS to run the Linux cluster benchmarks"
fi
