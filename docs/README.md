# Documentation

Start with the [project overview](../README.md), then follow the path that matches
what you want to inspect.

| Document | What it answers |
| --- | --- |
| [Architecture](architecture.md) | Who owns each piece of state? When is a write durable, committed, and applied? How do reads and recovery work? |
| [Design decisions](design-decisions.md) | Why this structure, what alternatives exist, and what does each choice cost? |
| [Demo](demo.md) | How do I run three nodes, find the leader, issue commands, and reproduce failure behavior? |
| [Validation](../VALIDATION.md) | Which tests and process scenarios run, what evidence exists, and what remains untested? |

## Code map

The source tree follows runtime responsibilities. `NodeRuntime` wires the modules
together; the consensus core accepts messages and exposes work to its caller.

| Path | Responsibility | Start here |
| --- | --- | --- |
| [`src/app/`](../src/app/) | Three event loops, startup recovery, requests, apply handoff, snapshot coordination, shutdown. | [`node_runtime.cpp`](../src/app/node_runtime.cpp) |
| [`src/raft/`](../src/raft/) | Elections, replication, quorum commit, ReadIndex, log compaction. | [`raft.h`](../src/raft/raft.h), [`raft.cpp`](../src/raft/raft.cpp) |
| [`src/server/`](../src/server/) | RESP parser/sessions and deterministic KV command application. | [`resp_server.cpp`](../src/server/resp_server.cpp), [`kv_store.cpp`](../src/server/kv_store.cpp) |
| [`src/transport/`](../src/transport/) | Peer connections, frame codec, reconnect and outbound queues. | [`peer.cpp`](../src/transport/peer.cpp), [`proto.h`](../src/transport/proto.h) |
| [`src/wal/`](../src/wal/) | Record encoding, checksums, buffered writes, fsync, and recovery. | [`wal.cpp`](../src/wal/wal.cpp), [`proto.h`](../src/wal/proto.h) |
| [`src/common/`](../src/common/) | Bounded byte-buffer support. | [`bytebuffer.h`](../src/common/bytebuffer.h) |
| [`tests/`](../tests/) | Consensus, storage, protocol, and socket regression tests. | [Property-to-test map](../VALIDATION.md#property-to-test-map) |
| [`scripts/`](../scripts/) | RESP client and the real three-process validator. | [`validate_cluster.sh`](../scripts/validate_cluster.sh) |

For a write, follow `begin_write` → `Raft::propose` →
`handle_append_entries_response` → `dispatch_next_committed_entry` →
`apply_committed_entry` → `acknowledge_applied`. For a read, follow `begin_read` →
`Raft::read_index` → `read_index_ready` → `complete_read`.
