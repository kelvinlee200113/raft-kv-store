# Run the three-node demo

[Overview](../README.md) · [Architecture](architecture.md) · [Validation](../VALIDATION.md)

Build the binary with the commands in the [README](../README.md#build-and-verify).
Run commands below from the repository root. The examples use numeric IPv4
addresses, localhost ports, and small values.

## Automated failure walkthrough

```sh
KEEP_VALIDATION_DATA=1 ./scripts/validate_cluster.sh
```

The validator selects temporary ports and prints the directory containing its
node logs and WAL data. It stops its own processes at exit and retains the files
when `KEEP_VALIDATION_DATA=1` is set. Without that setting, it also removes its
temporary files.

| Stage | What to observe |
| --- | --- |
| Election and commands | One elected leader; `SET` succeeds and ReadIndex `GET` returns the committed value. |
| Concurrent clients | 16 writes complete by default and each stored value is read back. |
| Follower routing | Both followers reject `GET`, `SET`, and `DEL`. |
| Leader crash | The script kills the tracked leader process; a different leader completes writes and reads. |
| Full restart | Every node reports recovered term/commit metadata; at least one recovered snapshot is reported. |
| Deletion and restart | A committed deletion remains absent and an independent value survives. |
| Majority loss | A lone node cannot successfully complete a new write or ReadIndex read; reads recover after quorum returns. |

The final success line is:

```text
[raft-validation] PASS: election, quorum safety, leader-only reads, failover, snapshot recovery, and deletion durability
```

This exercises process failure on one machine. It does not inject arbitrary
network partitions, hardware power loss, or all disk failures.

To validate a sanitizer or another build:

```sh
RAFT_KV_BIN="$PWD/build-address/raft-kv" KEEP_VALIDATION_DATA=1 ./scripts/validate_cluster.sh
```

## Manual cluster

Start one node in each of three terminals. `mktemp` gives each process fresh data
and avoids accidentally mixing a new demo with a previous WAL.

Terminal 1:

```sh
node_data=$(mktemp -d /tmp/raft-kv-node1.XXXXXX)
./build/raft-kv --id=1 --raft=127.0.0.1:9101 --client=127.0.0.1:9201 \
  --peer=2@127.0.0.1:9102 --peer=3@127.0.0.1:9103 --data="$node_data"
```

Terminal 2:

```sh
node_data=$(mktemp -d /tmp/raft-kv-node2.XXXXXX)
./build/raft-kv --id=2 --raft=127.0.0.1:9102 --client=127.0.0.1:9202 \
  --peer=1@127.0.0.1:9101 --peer=3@127.0.0.1:9103 --data="$node_data"
```

Terminal 3:

```sh
node_data=$(mktemp -d /tmp/raft-kv-node3.XXXXXX)
./build/raft-kv --id=3 --raft=127.0.0.1:9103 --client=127.0.0.1:9203 \
  --peer=1@127.0.0.1:9101 --peer=2@127.0.0.1:9102 --data="$node_data"
```

Each process prints its data directory and state transitions. Wait for a
`state=Leader` line, then use that node's client port. The member-to-port mapping
in this example is `1 → 9201`, `2 → 9202`, `3 → 9203`.

In a fourth terminal, if node 1 is the leader:

```sh
python3 scripts/resp_client.py --port 9201 PING
python3 scripts/resp_client.py --port 9201 SET instrument ES
python3 scripts/resp_client.py --port 9201 GET instrument
python3 scripts/resp_client.py --port 9201 DEL instrument
python3 scripts/resp_client.py --port 9201 GET instrument
```

Expected replies are `PONG`, `OK`, `ES`, `1`, and `(nil)`. Replace `9201` if
another member is leader. `NOT_LEADER 2`, for example, means retry routing to
node 2's client port; the helper does not redirect automatically.

Stop a node with Ctrl-C. To test restart, rerun its exact command in the same
terminal with the same `$node_data`. To test leader loss, stop the current leader,
wait for a new `state=Leader` line in a surviving terminal, and address that node.
Use the automated walkthrough for the tracked-PID abrupt-crash scenario.

When finished, Ctrl-C each running node. Manual-demo data remains at the printed
directories so you can inspect or reuse it.

## Common surprises

- **`PING` succeeds but `GET` fails:** local process health does not prove a majority.
- **`TRYAGAIN write outcome unknown`:** the request timed out; it might commit later.
- **No leader:** all three nodes need matching peer addresses; at least two must
  communicate. Check node logs and that the six example ports are available.
- **`Address already in use`:** stop the previous demo processes or choose another
  consistent set of peer and client ports. The automated validator selects ports.
- **Snapshot size failure:** this revision captures whole state in one WAL record.
  Keep the dataset small; see [capacity boundaries](architecture.md#storage-and-capacity-boundaries).
