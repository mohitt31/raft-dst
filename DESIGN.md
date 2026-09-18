# Design

This file records each significant decision: what I picked, what I rejected
and why. Entries are written when the decision is made. When a measurement
settles a question, the number lives in NUMBERS.md and is linked from here.

Status tags: **[built]** implemented and tested, **[decided]** not built yet.

---

## 1. The Raft engine does no I/O [built]

`raft::Core` is a pure state machine. It never reads a clock, touches a
socket or writes a file. Inputs are `on_message`, `on_timer` and `propose`,
each carrying `now`. Outputs collect in a `Ready` batch: hard state and log
entries to persist, messages to send, and the newly committed range.

The interfaces are in `src/env/`: `IClock` (now + one wakeup slot),
`INetwork` (best-effort send; receive is push), `IStorage` (buffered writes,
`sync`, `load`) and `INode` (`on_frame`, `on_wake`), all handed to a node
through one `NodeContext`. The simulator implements them today; the real
event loop will implement the same four.

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

## 2. Timers are deadlines, not ticks [built]

Core exposes `next_deadline()`; the host arms one wakeup per node.

**Rejected: a fixed logical tick (etcd style).** A 10 ms tick on five nodes
costs 1.8 M events per simulated cluster-hour even when the cluster is idle,
which directly caps simulated hours per wall minute. With deadlines an idle
cluster costs roughly one heartbeat per interval.

## 3. Determinism rules [built]

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
| uninitialised reads, pointer values, per-process state | — | same seed run twice in one process **and** in two separate processes |
| tracing perturbs behaviour | — | trace on vs off must give the same final digest |

**Why two processes.** Two runs inside one test process usually get the same
heap layout, so a pointer value leaking into behaviour goes unnoticed. I
checked this: with a heap address mixed into every delivery trace record, the
in-process determinism test still passed and the two-process test
(`sim.cross_process_determinism`) failed. Both stay.

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

## 6. Log layout: struct of arrays, index = position [built]

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

**One container for memory and disk.** `EntryLog` (in `common/`, not `raft/`)
is used both as Raft's in-memory log and as the durable copy inside
`SimStorage`, so it is written and differential-tested once. Slices
(`LogSlice`) hold the log pointer plus indices, not spans: appends may
reallocate the arrays without invalidating a slice, while truncation bumps a
generation counter that DCHECK builds verify on every slice access. The
differential test runs random append / bulk append / truncate / slice
against a `vector<pair<Term, vector<uint8_t>>>` model and compares every
observable after every step. `ends_` is `uint32_t`, so one log's payloads are
capped at 4 GiB (asserted); simulated runs are far below that.

Index equals position only because compaction is out of scope. Log memory
grows without bound; that is accepted by the cut list.

## 7. Leader state in fixed arrays [built]

`std::array<Progress, kMaxNodes>` with `{next, match, last_sent, mode}`, and
votes as a bitmask. Commit index: insertion-sort at most five match indices
and take the quorum element, committing only if that entry's term is the
current term (paper Figure 8).

The simulator and each real node are single-threaded, so false sharing
cannot occur. Nothing is padded to cache-line size; layout is about density
and avoiding indirection.

## 8. Message ownership [decided; views built]

Outgoing AppendEntries carry a non-owning slice into the log that is valid
only until the next mutating call on Core. The host encodes it to bytes in
the same step. Debug builds check this with a generation counter. Incoming
frames decode into views over the receive buffer; Core copies payload bytes
into its arena only when it appends them.

The simulator carries encoded bytes, not structs. That costs throughput, but
the codec then runs under every chaos run and the TCP path uses exactly the
same bytes. Simulator benchmark numbers include codec cost.

## 7d. Key-value state machine [built]

`IStateMachine` (in `env/`, so `kv` does not depend on `node`) has `valid`,
`apply` and `digest`. `NodeHost` owns one per incarnation. KV state is
volatile: after a restart the node replays committed entries from index 1 as
it relearns the commit index, because snapshots are out of scope.

**Store:** `unordered_map<string, string>` with heterogeneous `string_view`
lookup, so gets and deletes build no temporary string. The hasher is our own
FNV-1a, not `std::hash`, whose values differ between standard libraries; the
lint bans `std::hash` for that reason. The map is never iterated, so bucket
order cannot reach a decision or a trace.

**Digest = content hash:** the XOR of a length-prefixed hash of each
(key, value) pair, updated incrementally on put and delete. It depends on
contents only, not on the order things were applied, so two nodes that
applied the same commands in the same order can be compared with one integer,
and a test asserts a rebuilt store matches a differently-ordered history.

**Commands** are `put` and `delete`, length-prefixed and strictly decoded, so
each valid command has exactly one encoding (same rule as the wire codec, and
the same two property tests). Empty payloads are reserved for leader no-ops.
`NodeHost::propose` validates a command before proposing, so every committed
non-empty entry decodes.

## 7e. Client workload [built]

Open loop: the harness advances virtual time to the next client operation and
proposes whether or not earlier ones finished, so a stalled cluster does not
slow the client down (the same coordinated-omission rule the real benchmark
will use). It aims at the node it last saw lead, otherwise at a random node,
and follows NotLeader hints. No sessions and no retries (both cut), so a lost
write is counted rather than repeated.

A proposal is acknowledged when the entry at the returned index is committed
**with the returned term**. A different term at that index means the write
was lost during a leader change, which is correct behaviour without client
sessions.

Client calls enter through `World::call_node`, which wraps them in the same
crash handling as network callbacks: a proposal can die inside `sync()` too,
and then the node must stop there like any other crash.

## 7f. Continuous state-machine comparison [built]

The checker keeps a reference model: an ordered `std::map` that applies each
committed command in index order, with the digest recomputed from scratch
after every entry. Every node reports its own incrementally maintained digest
after each apply, and the two must match at every index on every node. That
makes the KV store a differential test against a model, running inside every
chaos run rather than only in a unit test.

## 7g. Commit liveness [built]

`run_scenario` stops faults at a set time; the last crashed node is back by
`faults_until + max down time`; after a settle window, proposals made from
then on must be acknowledged before the run ends (20 of them by default), and
every node must be up and agree on a leader. Like the election liveness check
this is a bounded-time proxy, not a proof.

One test failure here was the test's fault, not Raft's: the first version
compared every node's applied index at the instant writes stopped, but a
follower learns a new commit index on the next heartbeat, up to 50 ms later.
The test now stops writing, runs one quiet second, and then requires exact
convergence and zero pending writes.

## 8a. Message and wire format [built]

**One flat `Message` struct tagged by `MsgType`**, fields unused by a type
left zero (the etcd `raftpb.Message` shape). Rejected: `std::variant` of four
structs. Core's handling is a switch on type either way; the flat struct
keeps the outbox a plain reused vector and lets tests build any message by
setting fields. The candidate/leader id is not a field: it is `from`.

**Entries are a struct of arrays on the wire too:** `u32 count`, then
`count` fixed 12-byte `(u64 term, u32 len)` records, then all payloads back
to back. `term(i)` on a decoded frame is O(1) with no copy, and encoding from
an `EntryLog` slice writes the payloads with one bulk copy. `EntriesView`
hides whether entries come from a log slice or from frame bytes; the
round-trip test is a differential test of those two backings.

**The decoder is the trust boundary.** It rejects anything a correct node
cannot send: node ids outside the cluster, `from == to`, term 0, a log
position with index 0 xor term 0, a position or entry term above the
message term, decreasing entry terms, index overflow, booleans other than
0/1, hints on a successful response, missing hints on a rejection, trailing
bytes. Core can then rely on those facts instead of re-checking them, and a
corrupted frame can never reach the `EntryLog` term assertion.

Strictness has a checkable consequence: every accepted frame has exactly
one encoding. Two property tests assert `encode(decode(bytes)) == bytes`, on
structured random frames (about a tenth pass validation, which the test
requires so it cannot pass vacuously) and on bit-flipped valid frames. A
mutant decoder that reads any non-zero byte as `true` fails both, plus the
hand-written case.

No checksum in the frame. TCP's checksum is weak, but the cut list rules
out designing for a hostile or corrupting network, and the simulator does
not inject corruption. The WAL does carry CRC-32C, because torn writes are
in scope.

Frames are length-prefixed so the same bytes work as simulator datagrams and
on a TCP stream; `probe_frame` finds frame boundaries and rejects length
prefixes above 16 MiB before any allocation.

## 9. Simulator event queue [built]

24-byte trivially copyable events `{at, seq, payload, kind, node}` in a
binary min-heap keyed on `(at, seq)`. Payload bytes live in a pool of
recycled buffers. Cancelled timers are dropped lazily via a per-node timer
generation.

**Rejected: `std::function` callbacks** — allocation, indirect call, and
nothing for the trace to inspect.

Differential-tested against a linear-scan model with times drawn from a
16-value range, so equal times are the common case. Message bytes sit in a
slot pool with a free list; on delivery the bytes are swapped into a
dedicated receive buffer, because the node may send while handling the
frame and growing the pool must not move the bytes it is reading.

## 10. Fault model [built]

Network: drop, delay (base + jitter + rare long tail, which gives
reordering), duplication, symmetric partitions and one-way link cuts. Nodes:
crash and restart, with unsynced writes lost, and crashes both between
events and inside `sync()` so only a prefix of the write survives. All rates
are integer parts-per-million.

Partitions come in three shapes: one node isolated both ways, a random split
into two non-empty groups, and one direction of one link cut. Messages are
checked against partitions both when sent and when delivered. After
`faults_until` no new faults start, links heal, armed in-sync crashes are
disarmed and drops/duplicates/long delays stop; crashed nodes still restart
on schedule. That quiet window is what the liveness check will use.

Scenario tests get scripted controls (`crash_node`, `restart_node`,
`set_link`) in addition to the random schedule.

Duplication is not in the original spec list. I added it because it is
about ten lines and exercises a real bug class: a duplicated, stale
AppendEntries truncating entries the follower already acknowledged.

## 7a. Core behaviour choices [built]

**Assert on our own state, never on input.** `RAFT_ASSERT` in the Core
guards the Core's own invariants (checked after every input in DCHECK
builds: commit within the log, leader log ends in its own term, every peer's
`match < next <= last + 1`, and so on). Input that a correct peer could never
send, like an AppendEntries from another leader of our own term or a request
to truncate a committed entry, is ignored and counted in `CoreStats`. A buggy
peer (the mutants build plants exactly those) must not abort the process that
is observing it: one abort would end a whole fuzz sweep, and the global
checker, which sees every node, reports the violation with its seed instead.

**Election timer resets** only as the paper says: on an AppendEntries from
the current leader, on granting a vote, and when starting an election. A
leader or candidate stepping down starts a fresh timer because it had none.
Stepping down to a higher term *without* granting a vote does not reset a
follower's timer, so a node with a stale log that keeps calling elections
cannot hold back the up-to-date nodes that could win.

**No-op on election.** A new leader appends an empty entry in its own term
(thesis section 6.4). Entries from earlier terms can then commit without
waiting for a client write, which matters for failover time. Empty payloads
are reserved for this; `NodeHost::propose` rejects empty commands.

**Commit rule.** Insertion-sort at most five match indices, take the quorum
element, and commit only if that entry is from the current term (Figure 8).
Scenario test: an earlier-term entry replicated to a majority stays
uncommitted until a current-term entry reaches a majority.

**Follower truncation** happens only at the first index whose term differs.
A late, shorter AppendEntries whose entries are all present never removes
the newer entries behind them; this has its own scenario test. A follower's
commit moves to `min(leader_commit, prev_index + entries)`, never to its own
last index, and never backwards.

**Flow control: probe and replicate per peer (etcd's model).** A new leader
probes every peer: one AppendEntries, then wait for a response or the next
heartbeat. A success moves the peer to replicate mode, where sends are
pipelined and `next` advances optimistically. New entries go to replicating
peers when the host collects `Ready`, not inside `propose`, so N proposals
in one step share one AppendEntries per peer and one fsync. A rejection
moves back to probe, using the follower's conflict hint to skip a whole
term. If a heartbeat interval passes with no progress while optimistic sends
are outstanding, they are assumed lost and the peer drops back to probing
from `match + 1`. My first test for this expected the fallback at the first
heartbeat; tracing it showed an ack had arrived inside that interval, so the
code was following the rule and the test was wrong. The test now pins
exactly one quiet interval.

## 7b. NodeHost [built]

`NodeHost` is the only glue between Core and the environment, shared by the
simulator and `raftd`. Each input is one step and one drain: write hard
state, then log entries, sync, send, apply, `advance()`, re-arm the timer.
Hard state is written before entries in the same sync so a torn write can
never keep entries from a term whose term bump was lost (the Core asserts on
load that no log entry is above the persisted term).

Decoding happens here, and a frame whose header `from`/`to` disagree with
the transport is dropped. The `IObserver` hooks report only facts that are
already durable: recovered state, hard state after sync, leadership after
the vote is synced, and commits as they are applied.

## 7c. Safety checker [built]

`harness::Checker` observes every node and checks, on every report rather
than at the end of a run: at most one leader per term; a node's durable vote
never changes or disappears within a term, including across restarts; a
node's durable term never decreases; every node commits the same (term,
payload) at each index; a new leader's log holds every committed entry.
Violations are recorded with time and node, never asserted.
`SimCluster::check_node_stats` adds the Core's refused-input counters and
the host's undecodable or misrouted frame counts, which must all be zero.

**The checker had a bug of its own.** On a restart into a newer term with no
vote, it kept the vote recorded for the older term, so a legitimate vote
later in the new term would have been reported as a changed vote. I found it
reading the code before any test hit it. `RestartIntoNewTermWithoutVote...`
is its regression test.

**Planted bugs it catches** (manual check before the mutants preset exists):
granting a vote regardless of an earlier vote in the same term is reported
on seed 1 as "vote in term 26 changed from n2 to n0", before a second leader
is even elected; persisting term but not the vote is reported on seed 1 as
"vote in term 1 changed from n1 to none (on restart)".

**Liveness, for elections.** The chaos property test stops faults at 30 s;
the last crashed node is back by 33 s (maximum down time); the cluster then
gets 15 s, 50 maximum election timeouts, and must end with every node up,
in one term, naming one leader. 40 seeds on 3- and 5-node clusters. This is
a bounded-time check, not a proof: a long enough run of split votes would
fail it, and then the seed gets investigated.

## 10a. A crash inside sync() throws [built]

`SimStorage::sync()` with a crash armed keeps a random prefix of the staged
writes and throws `SimCrash`. The world catches it at the node callback
boundary and destroys the node.

**Rejected: mark the node dead and let the callback finish.** Everything the
node does after the crash point would still happen: sends (easy to filter),
but also applying committed entries and reporting them to the checker, which
a process that died inside fsync can never do. An exception stops the node
exactly where a real process stops. The cost is that node code must be
exception-safe, which RAII gives for free, and the exception only exists in
the simulator.

Crash-legal disk states are counted in units: one per `set_hard_state`, one
per appended entry. An append's truncation lands only together with its
first entry, matching a WAL where the truncation point lives in the entries
record. A property test checks the storage against an independent model:
after a normal sync the disk equals all staged writes, after a plain crash it
is unchanged, after a crash inside sync it equals some prefix. A mutant that
silently drops one hard-state write during a torn sync fails that test with a
replay seed.

The world test checks from the other side: with every injected crash placed
inside a sync, no node ever sends at the instant it died.

## 11. Fuzzer, reducer and mutants [built]

**Each seed draws its own configuration (swarm testing).** `FuzzCase`
randomises node count, batch size, election and heartbeat timing, network
delay bounds, every fault rate, client rate and key count, then prints them
as one parseable line. Fixing the configuration across seeds hides bugs: with
batches of up to 256 entries a new leader's no-op almost always travels with
the entries before it, which is exactly the gap the paper's Figure 8 case
needs.

**Reducer.** Changing any knob changes the whole execution, so a candidate is
kept only if it still fails *the same way*. The signature is the violation
text with digits stripped, which drops times, node ids and indices but keeps
the wording. It shrinks the run length, then node count, then whole fault
classes, then rates, then the key space.

**Regression corpus.** `tests/regressions/*.repro` holds reduced cases, one
per line; a CTest case replays all of them. It is empty so far: the fuzzer
has not found a defect in the engine yet (campaign counts in NUMBERS.md).

**Mutants: measuring whether the checker has teeth.** The `mutants` preset
compiles six planted bugs, guarded so that in every other build
`Core::mutation_on` is a compile-time false and the branches vanish. The
release binary contains none of their strings, which is checked directly with
`strings`.

The measured result (NUMBERS.md has the commands):

| Planted bug | Seeds of fuzzing to find it |
|---|---|
| vote without the up-to-date log check | 1 |
| a second vote in one term | 2 |
| truncate at prev_index without a conflict | 1 |
| persist the term but not the vote | 1 |
| commit earlier-term entries by counting (Figure 8) | 194 |
| follower commit not capped at what a message verified | 1561 |

The spread across three orders of magnitude is the point. Four bugs die on
the first or second seed. The other two need a specific shape of history:
Figure 8 needs a majority holding an old-term entry while the new leader's
no-op is missing from it, and the unbounded follower commit needs a follower
with a stale divergent tail receiving an append that matches further back
than the leader's commit index.

So each of those two also gets a targeted scenario driven straight against
the Core, which catches it in under a millisecond instead of minutes of
search. The mutants test uses a 60-seed budget for speed and requires every
planted bug to be caught by fuzzing **or** by its scenario, so an escape
fails the build. When I first ran it, `follower-commit-unbounded` escaped both
and the test failed: that is how the missing scenario was found. I had also
written earlier that random search could not reach these two at all; the
2000-seed measurements above corrected that.

Two side findings:

* With internal DCHECKs on, the double-vote mutant aborts inside the Core's
  own invariant (`candidate did not vote for itself`) before the checker can
  report. That is defence in depth working, but it stops a sweep, so the
  mutants preset runs with DCHECKs off and measures the external checker.
* The determinism lint flagged the string literal `"double-vote"` as the
  banned type `double`. It matches inside string literals, which is blunt;
  the label was renamed to `vote-twice` rather than weakening the pattern.

## 11a. The first defect the fuzzer found was in the checker [built]

A 2700-seed campaign on the clean engine reported one failure, seed 2974:
*"leader of term 23 is missing committed entry 661"*. The trace showed:

    8303173392 n3 LEADER term=24 last=661 term@661=24
    8336883893 n3 COMMIT index=661 term=24      (then n4, n1, n0: a majority)
    8386270111 n2 LEADER term=23 last=661 term@661=23

n2 won term **23** after term 24 had already committed entry 661. That is
legal: n2 had started a term-23 election earlier and its votes were granted
before those nodes moved to term 24, so it won a stale term late. Such a
leader can never replicate anything, because every peer at term 24 rejects
it and sends its own term back.

Leader Completeness (paper section 5.4.3) says an entry committed in term T
is present in the logs of leaders of terms **greater than T**. My checker
ignored the term ordering and demanded every new leader hold every committed
entry, so it was a false positive: the engine was right and the checker was
wrong.

**Fix.** `on_commit` now also reports the committing node's current term,
which is an upper bound on the term the entry became committed in (the
checker keeps the tightest bound it sees). A leader of term T is only
required to hold entries whose commit term is below T. Using an upper bound
can only exempt a leader that a perfect check would have examined, so the
check can still be too weak but never raises a false alarm. All six planted
bugs are still caught after the change, including the two whose symptom is
exactly "leader is missing committed entry".

The case is committed as `tests/regressions/checker-stale-leader-term.repro`,
in both its original and reduced form.

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
