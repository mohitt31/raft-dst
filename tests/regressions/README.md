Each `.repro` file holds one fuzz case per line, in the format
`raft_fuzz --case` accepts. A case lands here when the fuzzer finds a
violation and `raft_reduce` has shrunk it. `ctest` replays every case and
requires it to pass, so a fixed bug cannot come back quietly.

Replay one by hand:

    build/release/tools/raft_fuzz --case "$(head -1 tests/regressions/<file>.repro)"
