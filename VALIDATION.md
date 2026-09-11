# Validation

[Overview](README.md) · [Architecture](docs/architecture.md) · [Demo](docs/demo.md)

The required acceptance path is a clean C++20 build, all CTest targets, and the real three-process cluster workflow.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 60 --no-tests=error
./scripts/validate_cluster.sh
```

## Sanitizers

Use separate build directories. Set `sanitizer` to `address`, `undefined`, or `thread`.

```bash
sanitizer=address
cmake -S . -B "build-$sanitizer" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRAFT_SANITIZER="$sanitizer"
cmake --build "build-$sanitizer" --parallel
ctest --test-dir "build-$sanitizer" --output-on-failure --no-tests=error
```

In the same terminal, run the process validator with the instrumented executable:

```bash
RAFT_KV_BIN="$PWD/build-$sanitizer/raft-kv" ./scripts/validate_cluster.sh
```

## Source coverage

Coverage uses Clang source instrumentation. Give each process a separate raw profile so the three-node workflow cannot overwrite another process's data.

```bash
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DRAFT_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel
mkdir -p build-coverage/profiles

LLVM_PROFILE_FILE="$PWD/build-coverage/profiles/%p-%m.profraw" \
  ctest --test-dir build-coverage --output-on-failure --no-tests=error

LLVM_PROFILE_FILE="$PWD/build-coverage/profiles/%p-%m.profraw" \
  RAFT_KV_BIN="$PWD/build-coverage/raft-kv" \
  ./scripts/validate_cluster.sh

llvm-profdata merge -sparse build-coverage/profiles/*.profraw \
  -o build-coverage/coverage.profdata

llvm-cov report build-coverage/raft-kv \
  --instr-profile=build-coverage/coverage.profdata \
  --object=build-coverage/voting_test \
  --object=build-coverage/replication_test \
  --object=build-coverage/kv_store_test \
  --object=build-coverage/transport_proto_test \
  --object=build-coverage/network_test \
  --object=build-coverage/read_index_test \
  --object=build-coverage/async_apply_test \
  --object=build-coverage/wal_test \
  --object=build-coverage/snapshot_test \
  --object=build-coverage/resp_codec_test \
  --object=build-coverage/resp_server_test \
  --show-branch-summary \
  --ignore-filename-regex='(^|/)main\.cpp$' \
  --sources src
```

Report only project source by passing `src/` to `llvm-cov`; exclude tests, generated files, dependencies, and system headers. On macOS, invoke the LLVM tools through `xcrun`.

## Local verification: 2026-09-11

Verified source revision: `9e018446a96a591d481f3c6c56ef36d941f40ca7`.
This documentation restructure leaves `src/`, `tests/`, CMake, and CI unchanged.
The checks below used a fresh isolated checkout of that source revision.

Environment: macOS 15.3.1, ARM64, AppleClang 17.0.0, CMake 4.0.3.

| Check | Result |
| --- | --- |
| Fresh Release configure and build | Passed |
| CTest, with a 60-second per-target timeout | 11/11 targets passed |
| Three-process validator with retained logs | Passed |
| Concurrent application scenario | 16 writes and readbacks passed |
| Leader crash and two-node failover | Passed |
| Full restarts and committed deletion | Passed |
| WAL metadata recovery | Reported by all three nodes after restart |
| Snapshot recovery | At least one nonzero recovered snapshot reported |
| No-majority write/read failure and read recovery after quorum returns | Passed |

The snapshot catch-up unit scenario is in `snapshot_test.cpp`; the process
validator establishes restart recovery, not every possible InstallSnapshot
interleaving. The no-majority scenario stops both followers rather than injecting
packet loss or an asymmetric partition. A timed-out write may later commit.

Sanitizers and coverage were not rerun for this documentation change; their
previous results remain dated below. This pass makes no performance claim.

The same source revision has a successful
[Linux GitHub Actions run](https://github.com/kelvinlee200113/raft-kv-store/actions/runs/32312245437).
The workflow runs a Release build, CTest, and the three-process validator; it does
not deploy a service or run the sanitizer/coverage configurations.

## Property-to-test map

| Property | Implementation | Verification |
| --- | --- | --- |
| Election, PreVote, and quorum commit | [`src/raft/`](src/raft/) | [`voting_test.cpp`](tests/voting_test.cpp), [`replication_test.cpp`](tests/replication_test.cpp) |
| Ordered single-flight application | [`node_runtime.cpp`](src/app/node_runtime.cpp), [`raft.cpp`](src/raft/raft.cpp) | [`async_apply_test.cpp`](tests/async_apply_test.cpp), process validator |
| Leader ReadIndex reads | [`raft.cpp`](src/raft/raft.cpp), [`node_runtime.cpp`](src/app/node_runtime.cpp) | [`read_index_test.cpp`](tests/read_index_test.cpp) |
| RESP framing and ordered sessions | [`src/server/`](src/server/) | [`resp_codec_test.cpp`](tests/resp_codec_test.cpp), [`resp_server_test.cpp`](tests/resp_server_test.cpp) |
| Peer framing and bounded queues | [`src/transport/`](src/transport/) | [`network_test.cpp`](tests/network_test.cpp), [`transport_proto_test.cpp`](tests/transport_proto_test.cpp) |
| WAL recovery and tail repair | [`src/wal/`](src/wal/) | [`wal_test.cpp`](tests/wal_test.cpp) |
| Snapshot compaction and catch-up | [`src/raft/`](src/raft/), [`src/wal/`](src/wal/) | [`snapshot_test.cpp`](tests/snapshot_test.cpp), restart scenarios in the process validator |
| Real process behavior | [`node_runtime.cpp`](src/app/node_runtime.cpp) | [`validate_cluster.sh`](scripts/validate_cluster.sh) |

## Historical local receipt: 2026-08-19

Measured on 2026-08-19 with AppleClang 17:

| Check | Result |
| --- | --- |
| Release | Build passed |
| CTest | 11/11 targets passed |
| Three-process validation | Passed |
| AddressSanitizer | 11/11 targets and three-process validation passed |
| UndefinedBehaviorSanitizer | 11/11 targets and three-process validation passed |
| ThreadSanitizer | 11/11 targets and three-process validation passed |
| Indicative source line coverage | 83.79% |
| Indicative source branch coverage | 71.08% |

Coverage shows which code executed; it is not proof of Raft safety. The public documentation makes no performance or production-readiness claim.

Apple LLVM 17 emits `warning: 50 functions have mismatched data` when this
report combines duplicated static-library mappings from the independently
instrumented executables. The percentages are reproducible with the commands
above, but are indicative rather than a precision quality gate.

The remaining uncovered code is concentrated in operating-system failure injection
(for example short writes, failed flushes, and repair failures), malformed startup
arguments and addresses, defensive rejection of invalid or out-of-sequence RPCs,
and rare socket cancellation paths. These paths remain explicit validation limits.
