# Technical decisions and tradeoffs

[Overview](../README.md) · [Architecture](architecture.md) · [Validation](../VALIDATION.md)

These notes describe choices visible in the current implementation and their
engineering consequences. They are a retrospective explanation, not a record of
benchmarks comparing all alternatives.

## 1. One static three-node group

**Choice.** One Raft group with members `{1,2,3}` and a two-node majority.
Configuration rejects missing, duplicate, or extra peers.

**Reasoning.** A fixed group keeps quorum formation, leader loss, and restart
behavior directly inspectable in a three-process demo. One replicated history
also gives the state machine a single application order.

**Alternative and cost.** Dynamic membership requires a safe configuration-change
protocol. Multi-Raft and sharding add routing, partition ownership, and multiple
independent consensus histories. The current design trades those scaling features
for a smaller correctness surface; adding machines does not increase write capacity.

**Revisit when:** membership must change online or data must be partitioned.

Evidence: [`parse_node_config`](../src/app/node_runtime.cpp),
[`Raft` configuration validation](../src/raft/raft.cpp),
[`voting_test.cpp`](../tests/voting_test.cpp).

## 2. Single owners connected by event-loop messages

**Choice.** Separate Raft/WAL, peer-network, and client/KV event loops; one thread
runs each loop. Committed application uses a single-flight handoff acknowledged
by log index.

**Reasoning.** Ownership makes the legal mutation paths explicit. The consensus
core can expose one next committed entry without maintaining another queue of
copied application work. The KV owner applies it before Raft advances.

**Alternative and cost.** One event loop would reduce handoffs but couple socket,
state-machine, and storage work more tightly. Shared mutable state protected by
locks would require reasoning about lock order and concurrent mutations. The
chosen design pays for posted work, copying/allocation, and serialized application.
The Raft log and pending client maps remain possible growth points.

**Revisit when:** profiling identifies handoff or apply throughput as the bottleneck.
Batching contiguous committed entries would still need an ordered completion rule.

Evidence: [`dispatch_next_committed_entry`, `acknowledge_applied`](../src/app/node_runtime.cpp),
[`async_apply_test.cpp`](../tests/async_apply_test.cpp).

## 3. Synchronous persistence on the Raft loop

**Choice.** Persist term/vote, log entries, and commit metadata at the relevant
consensus boundaries. `WAL::sync` performs `fwrite` → `fflush` → `fsync`.

**Reasoning.** Flush-before-response ordering makes durability dependencies visible
in the control flow. `fflush` matters because successful `fsync` cannot persist
bytes still sitting in stdio's userspace buffer. Storage failure prevents further
successful consensus progress through the affected node.

**Alternative and cost.** A dedicated storage worker or group commit could overlap
I/O and amortize flushes, but responses would need to wait for the correct durable
sequence and preserve ordering across failures. Today, a slow flush blocks Raft
message handling and timers, increasing response time and election disruption.

**Revisit when:** measured disk latency or flush frequency dominates the workload.
No group-commit or asynchronous-durability speedup is claimed.

Evidence: [`WAL::sync`](../src/wal/wal.cpp),
[`SyncSurvivesAbruptProcessExitWithoutFclose`](../tests/wal_test.cpp),
[`sync_wal`](../src/raft/raft.cpp).

## 4. Leader-only ReadIndex instead of read leases

**Choice.** `GET` waits for a quorum-confirmed ReadIndex round, a current-term
committed entry, and local application through the safe index.

**Reasoning.** A leader flag can be stale during a partition. Quorum confirmation
and the applied-index barrier address authority and local freshness separately.
Round contexts prevent a delayed response from confirming a different round.

**Alternative and cost.** Read leases can avoid an exchange while a lease is valid,
but require explicit timing assumptions. Follower reads need another coordination
or staleness contract. The current approach keeps the read contract focused while
paying for quorum communication and leader routing. A minority cannot serve a
successful new quorum-backed read.

**Revisit when:** read throughput or latency requires a measured alternative with
an explicit consistency model.

Evidence: [`read_index`, `read_index_ready`](../src/raft/raft.cpp),
[`read_index_test.cpp`](../tests/read_index_test.cpp).

## 5. Whole-state snapshots inside the WAL

**Choice.** Serialize the KV map with its applied index, persist it as one WAL
record, then compact the corresponding in-memory log prefix. Transfer the complete
snapshot to followers that are behind that prefix.

**Reasoning.** Capturing state and index on the KV owner avoids associating a newer
Raft index with older state. One record keeps snapshot recovery straightforward,
and the suffix-discard marker distinguishes installation from local compaction.

**Alternative and cost.** Separate snapshot files, chunked transfer, and segmented
WAL reclamation would support larger state and smaller restart scans, but require
atomic publication, partial-transfer handling, and coordination between snapshot
and log retention. Today, serialization copies the whole state, installation waits
across loops, and the encoded snapshot must fit the WAL's 24-bit payload length.
Oversized snapshots can stop the node in this revision. Old WAL files still grow.

**Revisit when:** retained disk usage, capture pauses, or state size exceeds the
small-dataset envelope. In-memory compaction alone does not solve those limits.

Evidence: [`maybe_capture_snapshot`, `restore_snapshot`](../src/app/node_runtime.cpp),
[`take_snapshot`, `handle_install_snapshot`](../src/raft/raft.cpp),
[`snapshot_test.cpp`](../tests/snapshot_test.cpp), [`wal_test.cpp`](../tests/wal_test.cpp).

## 6. Small RESP2 surface and explicit retry ambiguity

**Choice.** Support `PING`, `GET`, `SET`, and `DEL`; preserve request/reply order
within each client session. Separate sessions can proceed independently. Commands
use request identity for local response correlation.

**Reasoning.** A familiar wire format makes manual experiments easy while keeping
the replicated state-machine vocabulary small. A write timeout reports an unknown
outcome because accepting a proposal and sending the final reply are distinct events.

**Alternative and cost.** Full Redis compatibility would add substantial command
and semantic scope. Exactly-once retries would need durable client identities,
sequence numbers, cached results, and a retention policy. Repeating a `SET` can
interact with intervening writes; repeating a `DEL` can change the returned count.
The current request ID is not a durable deduplication contract.

**Revisit when:** an application requires transparent retries or a broader command
set. Define those semantics before extending the protocol.

Evidence: [`begin_write`, `expire_client_requests`](../src/app/node_runtime.cpp),
[`resp_server.cpp`](../src/server/resp_server.cpp),
[`resp_server_test.cpp`](../tests/resp_server_test.cpp).
