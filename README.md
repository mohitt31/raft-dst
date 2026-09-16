# raft-dst

A Raft key-value store built to be tested by deterministic simulation.

The Raft engine does no I/O. The same engine runs inside a single-threaded
simulator, where one seed controls every message drop, delay, crash and
election timeout, and on a real cluster over TCP with a write-ahead log.

Work in progress. No performance numbers are published yet; when they are,
each one will be in [NUMBERS.md](NUMBERS.md) with the command that
reproduces it.

## Build

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `release`, `asan` (AddressSanitizer + UBSan), `tsan`, `mutants`.

Design notes are in [DESIGN.md](DESIGN.md).
