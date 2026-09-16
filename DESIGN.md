# Design

This file records each significant decision: what I picked, what I rejected
and why. Entries are written when the decision is made. When a measurement
settles a question, the number lives in NUMBERS.md and is linked from here.

Status tags: **[built]** implemented and tested, **[decided]** not built yet.

---

## 1. The Raft engine does no I/O [decided]

`raft::Core` is a pure state machine. It never reads a clock, touches a
socket or writes a file. Inputs are `on_message`, `on_timer` and `propose`,
each carrying `now`. Outputs collect in a `Ready` batch: hard state and log
entries to persist, messages to send, and the newly committed range.

A thin `NodeHost` binds `Core` to `IClock`, `INetwork` and `IStorage`. The
same `NodeHost` runs inside the simulator and inside `raftd`. Only the
implementations of the three interfaces change.

**Rejected: Core calls the interfaces directly.** Every call out of Raft
logic is a place where a crash or a reentrant delivery can land halfway
through a step, so the simulator would have to model interleavings inside
Raft itself. Unit tests would need mocks, and the hot path gets virtual
calls. With a pure core, tests are plain function calls and determinism
follows from the construction.

**Host order per step:** persist → sync → send → apply → `ack_ready()`. A
vote grant or AppendEntries ack can never leave before the state it depends
on is durable. The simulator can crash a node inside `sync()`, so breaking
this order is something the checker can observe, not just a code comment.

## 2. Timers are deadlines, not ticks [decided]

Core exposes `next_deadline()`; the host arms one wakeup per node.

**Rejected: a fixed logical tick (etcd style).** A 10 ms tick on five nodes
costs 1.8 M events per simulated cluster-hour even when the cluster is idle,
which directly caps simulated hours per wall minute. With deadlines an idle
cluster costs roughly one heartbeat per interval.

## 3. Determinism rules [built: lint, generators; decided: rest]

Same seed must give byte-identical traces, and the target is identical
traces on macOS/libc++ and Linux/libstdc++ too. Hazards and how each is
handled:

| Hazard | Rule | Enforcement |
|---|---|---|
| `std::uniform_int_distribution`, `std::shuffle` are implementation-defined | own bounded draw (Lemire) and shuffle | known-answer tests |
| `std::hash` differs across standard libraries | never use its value for decisions or traces | lint |
| `unordered_map` iteration order | never iterate for decisions or traces | review + cross-platform trace test |
| floating point | no `float`/`double` in simulated code; probabilities are integer parts-per-million | lint |
| wall clock, threads, `rand`, `getenv` | banned in `common env raft kv node sim` | lint (`tests/lint/banned_apis.sh`) |
| equal-time events | queue key is `(time, insertion seq)`, which is unique | event queue tests |
| uninitialised reads | — | same-seed-twice trace comparison |
| tracing perturbs behaviour | — | trace on vs off must give the same final digest |

The lint has a self-test: every code line in `tests/lint/fixtures` must be
flagged, so a broken regex fails CI instead of silently passing.

**One seed, independent streams.** The master seed goes through splitmix64
to derive separate xoshiro256** streams for network faults, crashes, client
workload and each node's election jitter. All randomness still comes from
the one seed. The reason for splitting: the reducer switches fault classes
off one at a time. With a single shared stream, disabling crashes would
shift every later network draw and the failure would disappear for
unrelated reasons.

### 3a. Generators [built]

xoshiro256** seeded through SplitMix64. Bounded draws use Lemire's
multiply-and-reject method, written over a word type so the rejection rule is
tested exhaustively on 8-bit words (every bound, every first draw: accepted
outputs are exactly uniform). I checked that test by disabling rejection; it
fails. Probabilities are `chance_ppm(uint32_t)`, no floating point.

Tests check SplitMix64 against its published seed-1234567 sequence and compare
10,000 outputs of xoshiro against a line-by-line transcription of the
reference C code. A separate frozen-stream test pins the first outputs of
`Rng{42}` and `Rng::derive(42, 3)`. Those values were captured from the
implementation, so they prove stability, not correctness: every committed
regression seed depends on the streams never changing.

### 3b. Trace hash: FNV-1a 64 [built]

Chosen for a fixed public definition and published vectors, not speed.
Integers are absorbed as little-endian bytes. It hashes one byte at a time;
if trace hashing shows up in a simulator profile, it gets replaced by a
word-at-a-time hash and the swap is recorded here with the measurement.

### 3c. Byte encoding and CRC [built]

`ByteWriter`/`ByteReader` encode little-endian explicitly; no struct is ever
memcpy'd to the wire or disk. The reader has a sticky error flag: reads past
the end return zero or an empty span and set `ok() == false`, so a decoder
reads a whole record and checks once. A property test decodes every truncated
prefix of random encodings into exactly-sized buffers under ASan.

CRC-32C is table-driven software, checked against the standard check value
and the RFC 3720 vectors, and shown to catch every single-bit flip in a
64-byte record. Hardware CRC is on the out-of-scope list; the fsync behind
each WAL write costs far more than the checksum.

## 4. Strong types for Term, Index, NodeId, Time [built]

Term and Index are both `uint64_t` in the paper and swapping them is an easy
bug. Each is a separate 8-byte trivially copyable class with only the
operations Raft uses. `tests/unit/types_test.cc` asserts at compile time
that they do not convert, compare or add across types. `Time` and
`Duration` are signed nanoseconds: `Time - Time` gives a `Duration`,
`Time + Time` does not compile.

`NodeId` is one byte. Clusters are capped at five static nodes by the cut
list, so `kMaxNodes = 5` is a compile-time constant and per-peer state can
live in fixed arrays.

## 5. Checks [built]

`RAFT_ASSERT` is always compiled in, for conditions where continuing would
corrupt state or hide a safety violation. `RAFT_DCHECK` is for internal
invariants; it is on in the asan, tsan and mutants presets and off in
release, where the condition is type-checked but not evaluated.

## 6. Log layout: struct of arrays, index = position [decided]

```
terms_  : vector<Term>      terms_[0] = 0 sentinel
ends_   : vector<uint32_t>  end offset of entry i's payload
arena_  : vector<uint8_t>   all payloads back to back
```

Access patterns: `term_at(i)` on every AppendEntries and vote (hot, random),
tail append on every proposal, suffix truncation on conflict (rare), slice
`[next, next+k)` for sending (hot). Terms sit in a dense 8-byte array, so
matching and conflict backtracking never touch payload bytes. Truncation is
three `resize` calls. A send slice is a span into the arena.

**Rejected: `vector<Entry{Term, std::string}>`** — one allocation per entry,
a pointer chase per payload, and term scans spread across cache lines.
**Rejected: `deque`** — payloads are not contiguous.

Index equals position only because compaction is out of scope. Log memory
grows without bound; that is accepted by the cut list.

## 7. Leader state in fixed arrays [decided]

`std::array<Progress, kMaxNodes>` with `{next, match, last_sent, mode}`, and
votes as a bitmask. Commit index: insertion-sort at most five match indices
and take the quorum element, committing only if that entry's term is the
current term (paper Figure 8).

The simulator and each real node are single-threaded, so false sharing
cannot occur. Nothing is padded to cache-line size; layout is about density
and avoiding indirection.

## 8. Message ownership [decided]

Outgoing AppendEntries carry a non-owning slice into the log that is valid
only until the next mutating call on Core. The host encodes it to bytes in
the same step. Debug builds check this with a generation counter. Incoming
frames decode into views over the receive buffer; Core copies payload bytes
into its arena only when it appends them.

The simulator carries encoded bytes, not structs. That costs throughput, but
the codec then runs under every chaos run and the TCP path uses exactly the
same bytes. Simulator benchmark numbers include codec cost.

## 9. Simulator event queue [decided]

24-byte trivially copyable events `{at, seq, payload, kind, node}` in a
binary min-heap keyed on `(at, seq)`. Payload bytes live in a pool of
recycled buffers. Cancelled timers are dropped lazily via a per-node timer
generation.

**Rejected: `std::function` callbacks** — allocation, indirect call, and
nothing for the trace to inspect.

## 10. Fault model [decided]

Network: drop, delay (base + jitter + rare long tail, which gives
reordering), duplication, symmetric partitions and one-way link cuts. Nodes:
crash and restart, with unsynced writes lost, and crashes both between
events and inside `sync()` so only a prefix of the write survives. All rates
are integer parts-per-million.

Duplication is not in the original spec list. I added it because it is
about ten lines and exercises a real bug class: a duplicated, stale
AppendEntries truncating entries the follower already acknowledged.

## 11. Proving the checker has teeth: mutants [decided]

The `mutants` preset compiles switches that plant known Raft bugs: committing
entries from an older term by counting replicas, skipping the vote persist,
skipping the up-to-date log check on votes, truncating on stale
AppendEntries. A test requires the fuzzer to catch each within a fixed seed
budget. The switches do not exist in the release engine.

## 12. Real cluster [decided]

Three `raftd` processes on one bare-metal Linux box, each pinned to its own
isolated core. Latency and failover numbers from that setup are single-host
loopback numbers and are labelled as such. The deploy scripts take a host
list, so the same measurement runs on three machines if they become
available.

## 13. Build configurations [built]

| Preset | Flags | DCHECK |
|---|---|---|
| release | `-O2 -g -DNDEBUG` | off |
| asan | `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all`, libc++ extensive hardening / `_GLIBCXX_ASSERTIONS` | on |
| tsan | `-O1 -g -fsanitize=thread` | on |
| mutants | release flags + planted-bug switches | on |

Project code builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast` and more, as errors. Tests use a lighter
set because gtest macros trip conversion warnings. GoogleTest 1.17.0 is
fetched by URL with a pinned SHA-256; tests are discovered at ctest time so a
sanitizer abort is a failing test, not a broken build.

## Out of scope

From the spec's cut list: log compaction and snapshots, membership changes,
a real storage engine, clusters above five nodes, client sessions,
read-index linearizable reads.

Considered and left out to protect the schedule: pre-vote and CheckQuorum,
simulated clock drift, a separate fsync thread, epoll/kqueue/io_uring/AF_XDP,
hardware CRC32C, a full linearizability checker.
