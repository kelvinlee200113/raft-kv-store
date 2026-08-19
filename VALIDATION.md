# Validation

The required acceptance path is a clean C++20 build, all CTest targets, and the real three-process cluster workflow.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
./scripts/validate_cluster.sh
```

## Sanitizers

Use separate build directories. Replace `<kind>` with `address`, `undefined`, or `thread`.

```bash
cmake -S . -B build-<kind> \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRAFT_SANITIZER=<kind>
cmake --build build-<kind> --parallel
ctest --test-dir build-<kind> --output-on-failure --no-tests=error
```

Run `validate_cluster.sh` with the instrumented executable when validating the complete runtime:

```bash
RAFT_KV_BIN="$PWD/build-<kind>/raft-kv" ./scripts/validate_cluster.sh
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

## Latest local receipt

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
