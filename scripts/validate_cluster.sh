#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
raft_binary="${RAFT_KV_BIN:-${repo_root}/build/raft-kv}"
python_binary="${PYTHON_BIN:-python3}"
resp_client="${repo_root}/scripts/resp_client.py"
tick_ms="${RAFT_VALIDATION_TICK_MS:-20}"
snapshot_threshold="${RAFT_VALIDATION_SNAPSHOT_THRESHOLD:-2}"

if [[ ! -x "${raft_binary}" ]]; then
    echo "validation error: executable not found at ${raft_binary}" >&2
    echo "build it first, or set RAFT_KV_BIN=/absolute/path/to/raft-kv" >&2
    exit 1
fi
if ! command -v "${python_binary}" >/dev/null 2>&1; then
    echo "validation error: ${python_binary} is required" >&2
    exit 1
fi

tmp_parent="${TMPDIR:-/tmp}"
tmp_parent="${tmp_parent%/}"
validation_root=""

early_cleanup() {
    local status=$?
    if [[ "${KEEP_VALIDATION_DATA:-0}" != "1" && -n "${validation_root}" && -d "${validation_root}" ]]; then
        rm -rf -- "${validation_root}"
    fi
    trap - EXIT
    exit "${status}"
}

trap early_cleanup EXIT
validation_root="$(mktemp -d "${tmp_parent}/raft-kv-validation.XXXXXX")"
mkdir -p "${validation_root}/node1" "${validation_root}/node2" "${validation_root}/node3" "${validation_root}/logs"
: >"${validation_root}/.raft-kv-validation-owned"

pid1=""
pid2=""
pid3=""
log_run=0

say() {
    printf '[raft-validation] %s\n' "$*"
}

allocate_ports() {
    "${python_binary}" - <<'PY'
import socket

sockets = []
try:
    for _ in range(6):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
}

raft_port1=""
raft_port2=""
raft_port3=""
client_port1=""
client_port2=""
client_port3=""

assign_ports() {
    local ports
    local unexpected_port=""
    ports="$(allocate_ports)"
    read -r raft_port1 raft_port2 raft_port3 client_port1 client_port2 client_port3 unexpected_port <<<"${ports}"
    if [[ -n "${unexpected_port}" || -z "${client_port3}" ]]; then
        echo "validation error: failed to allocate six localhost ports" >&2
        return 1
    fi
}

pid_for() {
    case "$1" in
        1) printf '%s' "${pid1}" ;;
        2) printf '%s' "${pid2}" ;;
        3) printf '%s' "${pid3}" ;;
        *) return 1 ;;
    esac
}

client_port_for() {
    case "$1" in
        1) printf '%s' "${client_port1}" ;;
        2) printf '%s' "${client_port2}" ;;
        3) printf '%s' "${client_port3}" ;;
        *) return 1 ;;
    esac
}

clear_pid() {
    case "$1" in
        1) pid1="" ;;
        2) pid2="" ;;
        3) pid3="" ;;
        *) return 1 ;;
    esac
}

is_tracked_node_process() {
    local id="$1"
    local pid="$2"
    local command
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null || return 1
    command="$(ps -ww -o command= -p "${pid}" 2>/dev/null)" || return 1
    [[ "${command}" == *"--id=${id}"* &&
       "${command}" == *"--data=${validation_root}/node${id}"* ]]
}

is_alive() {
    local id="$1"
    local pid
    pid="$(pid_for "${id}")"
    is_tracked_node_process "${id}" "${pid}"
}

start_node() {
    local id="$1"
    local log_path="${validation_root}/logs/node${id}-run${log_run}.log"
    case "${id}" in
        1)
            "${raft_binary}" --id=1 --raft="127.0.0.1:${raft_port1}" --client="127.0.0.1:${client_port1}" \
                --peer="2@127.0.0.1:${raft_port2}" --peer="3@127.0.0.1:${raft_port3}" \
                --data="${validation_root}/node1" --tick-ms="${tick_ms}" --snapshot-threshold="${snapshot_threshold}" \
                >>"${log_path}" 2>&1 &
            pid1=$!
            ;;
        2)
            "${raft_binary}" --id=2 --raft="127.0.0.1:${raft_port2}" --client="127.0.0.1:${client_port2}" \
                --peer="1@127.0.0.1:${raft_port1}" --peer="3@127.0.0.1:${raft_port3}" \
                --data="${validation_root}/node2" --tick-ms="${tick_ms}" --snapshot-threshold="${snapshot_threshold}" \
                >>"${log_path}" 2>&1 &
            pid2=$!
            ;;
        3)
            "${raft_binary}" --id=3 --raft="127.0.0.1:${raft_port3}" --client="127.0.0.1:${client_port3}" \
                --peer="1@127.0.0.1:${raft_port1}" --peer="2@127.0.0.1:${raft_port2}" \
                --data="${validation_root}/node3" --tick-ms="${tick_ms}" --snapshot-threshold="${snapshot_threshold}" \
                >>"${log_path}" 2>&1 &
            pid3=$!
            ;;
        *)
            echo "validation error: invalid node id ${id}" >&2
            return 1
            ;;
    esac
}

start_cluster() {
    log_run=$((log_run + 1))
    start_node 1
    start_node 2
    start_node 3
}

stop_node() {
    local id="$1"
    local pid
    local attempt
    pid="$(pid_for "${id}")"

    if [[ -z "${pid}" ]]; then
        return 0
    fi

    if is_tracked_node_process "${id}" "${pid}"; then
        kill -TERM "${pid}" 2>/dev/null || true
        attempt=0
        while (( attempt < 100 )); do
            if ! is_tracked_node_process "${id}" "${pid}"; then
                break
            fi
            sleep 0.05
            attempt=$((attempt + 1))
        done
        if is_tracked_node_process "${id}" "${pid}"; then
            echo "validation error: node ${id} did not stop after SIGTERM; forcing exact PID ${pid}" >&2
            kill -KILL "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
            clear_pid "${id}"
            return 1
        fi
    fi

    if wait "${pid}"; then
        :
    else
        local wait_status=$?
        echo "validation error: node ${id} exited unexpectedly (status=${wait_status})" >&2
        clear_pid "${id}"
        return 1
    fi
    clear_pid "${id}"
}

crash_node() {
    local id="$1"
    local pid
    pid="$(pid_for "${id}")"
    if ! is_tracked_node_process "${id}" "${pid}"; then
        echo "validation error: cannot crash node ${id}; it is not running" >&2
        return 1
    fi
    kill -KILL "${pid}"
    wait "${pid}" 2>/dev/null || true
    clear_pid "${id}"
}

stop_cluster() {
    local failed=0
    stop_node 1 || failed=1
    stop_node 2 || failed=1
    stop_node 3 || failed=1
    return "${failed}"
}

show_logs() {
    local id
    local log_path
    for id in 1 2 3; do
        log_path="${validation_root}/logs/node${id}-run${log_run}.log"
        if [[ -f "${log_path}" ]]; then
            printf '\n--- node %s log, run %s (last 80 lines) ---\n' "${id}" "${log_run}" >&2
            tail -n 80 "${log_path}" >&2 || true
        fi
    done
}

cleanup() {
    local status=$?
    local stop_status=0
    stop_cluster || stop_status=$?
    if [[ "${status}" -eq 0 && "${stop_status}" -ne 0 ]]; then
        status="${stop_status}"
    fi
    if [[ "${status}" -ne 0 ]]; then
        show_logs
    fi

    if [[ "${KEEP_VALIDATION_DATA:-0}" == "1" ]]; then
        say "preserved validation data at ${validation_root}"
    elif [[ -f "${validation_root}/.raft-kv-validation-owned" ]]; then
        rm -rf -- "${validation_root}"
    else
        echo "validation warning: ownership marker missing; refusing to remove ${validation_root}" >&2
    fi

    trap - EXIT
    exit "${status}"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

request() {
    local id="$1"
    shift
    "${python_binary}" "${resp_client}" --port "$(client_port_for "${id}")" --timeout 3 "$@"
}

request_with_timeout() {
    local id="$1"
    local timeout="$2"
    shift 2
    "${python_binary}" "${resp_client}" --port "$(client_port_for "${id}")" --timeout "${timeout}" "$@"
}

wait_for_ping() {
    local id="$1"
    local deadline=$((SECONDS + 12))
    while (( SECONDS < deadline )); do
        if ! is_alive "${id}"; then
            echo "validation error: node ${id} exited during startup" >&2
            return 1
        fi
        if request "${id}" PING >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.05
    done
    echo "validation error: node ${id} did not accept RESP connections" >&2
    return 1
}

wait_for_cluster_ports() {
    wait_for_ping 1
    wait_for_ping 2
    wait_for_ping 3
}

start_cluster_with_fresh_ports() {
    local attempt
    for ((attempt = 1; attempt <= 5; attempt++)); do
        assign_ports
        start_cluster
        if wait_for_cluster_ports; then
            return 0
        fi
        say "startup attempt ${attempt} failed before all ports were ready; retrying with fresh ports"
        stop_cluster || true
    done
    echo "validation error: cluster startup failed after five port-allocation attempts" >&2
    return 1
}

find_leader() {
    local probe_value="$1"
    local timeout_seconds="$2"
    local deadline=$((SECONDS + timeout_seconds))
    local id
    local response

    while (( SECONDS < deadline )); do
        for id in 1 2 3; do
            if is_alive "${id}"; then
                if response="$(request "${id}" SET validation:leader-probe "${probe_value}" 2>/dev/null)" \
                    && [[ "${response}" == "OK" ]]; then
                    printf '%s' "${id}"
                    return 0
                fi
            fi
        done
        sleep 0.05
    done

    echo "validation error: no writable leader elected within ${timeout_seconds}s" >&2
    return 1
}

assert_reply() {
    local expected="$1"
    local description="$2"
    shift 2
    local actual
    actual="$("$@")"
    if [[ "${actual}" != "${expected}" ]]; then
        echo "validation error: ${description}: expected '${expected}', got '${actual}'" >&2
        return 1
    fi
    say "PASS: ${description} -> ${actual}"
}

assert_followers_reject_command() {
    local leader="$1"
    shift
    local command="$1"
    local -a request_arguments=("$@")
    local id
    local deadline
    local expected="NOT_LEADER ${leader}"
    local response=""
    local status=0
    local checked=0

    for id in 1 2 3; do
        if [[ "${id}" != "${leader}" ]] && is_alive "${id}"; then
            deadline=$((SECONDS + 5))
            while (( SECONDS < deadline )); do
                set +e
                response="$(request "${id}" "${request_arguments[@]}" 2>&1)"
                status=$?
                set -e
                if [[ "${status}" -eq 2 && "${response}" == "${expected}" ]]; then
                    say "PASS: follower ${id} rejected ${command} -> ${response}"
                    checked=$((checked + 1))
                    break
                fi
                if ! is_alive "${id}"; then
                    break
                fi
                sleep 0.05
            done
            if [[ "${status}" -ne 2 || "${response}" != "${expected}" ]]; then
                echo "validation error: follower ${id} did not reject ${command} with '${expected}' (status=${status}, reply='${response}')" >&2
                return 1
            fi
        fi
    done

    if [[ "${checked}" -ne 2 ]]; then
        echo "validation error: expected two live followers, checked ${checked}" >&2
        return 1
    fi
}

assert_concurrent_apply_handoff() {
    local leader="$1"
    local write_count="${RAFT_VALIDATION_STRESS_WRITES:-16}"
    local stress_dir="${validation_root}/apply-stress"
    local -a request_pids=()
    local id
    local response
    local failed=0

    mkdir -p "${stress_dir}"
    for ((id = 1; id <= write_count; id++)); do
        request "${leader}" SET "validation:stress:${id}" "value-${id}" \
            >"${stress_dir}/${id}.reply" 2>&1 &
        request_pids+=("$!")
    done

    for ((id = 1; id <= write_count; id++)); do
        if ! wait "${request_pids[$((id - 1))]}"; then
            failed=1
        fi
        response="$(<"${stress_dir}/${id}.reply")"
        if [[ "${response}" != "OK" ]]; then
            echo "validation error: concurrent write ${id} failed: '${response}'" >&2
            failed=1
        fi
    done
    if [[ "${failed}" -ne 0 ]]; then
        return 1
    fi

    for ((id = 1; id <= write_count; id++)); do
        assert_reply "value-${id}" "concurrent apply preserves key ${id}" \
            request "${leader}" GET "validation:stress:${id}"
    done
    say "PASS: ${write_count} concurrent writes completed without lost state"
}

assert_snapshot_recovered() {
    if ! grep -Eq 'recovered .* snapshot=[1-9][0-9]*' \
        "${validation_root}"/logs/node*-run"${log_run}".log; then
        echo "validation error: restarted nodes did not report a recovered snapshot" >&2
        return 1
    fi
    say "PASS: restart recovered a compacted snapshot"
}

assert_every_node_recovered_persisted_metadata() {
    local id
    for id in 1 2 3; do
        if ! grep -Eq "node ${id} recovered term=[1-9][0-9]* commit=[1-9][0-9]*" \
            "${validation_root}/logs/node${id}-run${log_run}.log"; then
            echo "validation error: node ${id} did not recover persisted consensus state" >&2
            return 1
        fi
    done
    say "PASS: all three nodes recovered persisted term and commit metadata"
}

assert_consensus_fails_without_majority() {
    local leader="$1"
    local id
    local restored_leader=""
    local response
    local status

    for id in 1 2 3; do
        if [[ "${id}" != "${leader}" ]]; then
            stop_node "${id}"
        fi
    done

    set +e
    response="$(request_with_timeout "${leader}" 7 SET validation:no-quorum rejected 2>&1)"
    status=$?
    set -e
    if [[ "${status}" -ne 2 || "${response}" != "TRYAGAIN write outcome unknown" ]]; then
        echo "validation error: no-majority write was not rejected safely (status=${status}, reply='${response}')" >&2
        return 1
    fi
    say "PASS: write without a two-node majority failed safely -> ${response}"

    set +e
    response="$(request_with_timeout "${leader}" 7 GET validation:control 2>&1)"
    status=$?
    set -e
    if [[ "${status}" -ne 2 || "${response}" != "TRYAGAIN read quorum unavailable" ]]; then
        echo "validation error: no-majority read was not rejected safely (status=${status}, reply='${response}')" >&2
        return 1
    fi
    say "PASS: ReadIndex without a two-node majority failed safely -> ${response}"

    stop_cluster
    start_cluster_with_fresh_ports
    restored_leader="$(find_leader quorum-restored 25)"
    assert_reply survives-final-restart \
        "ReadIndex succeeds through a fresh round after quorum returns" \
        request "${restored_leader}" GET validation:control
}

say "starting three nodes (data: ${validation_root})"
start_cluster_with_fresh_ports

leader="$(find_leader initial 20)"
say "elected leader: node ${leader}"
assert_reply OK "SET commits on the leader" request "${leader}" SET validation:key before-failover
assert_reply before-failover "ReadIndex GET returns the committed value" request "${leader}" GET validation:key
assert_concurrent_apply_handoff "${leader}"
assert_followers_reject_command "${leader}" GET validation:key
assert_followers_reject_command "${leader}" SET validation:follower-write rejected
assert_followers_reject_command "${leader}" DEL validation:key

say "crashing elected leader node ${leader} (exact PID $(pid_for "${leader}"))"
crash_node "${leader}"
new_leader="$(find_leader failover 25)"
if [[ "${new_leader}" == "${leader}" ]]; then
    echo "validation error: stopped node was reported as the new leader" >&2
    exit 1
fi
say "failover leader: node ${new_leader}"
assert_reply OK "SET commits after one-node failure" request "${new_leader}" SET validation:key after-failover
assert_reply after-failover "GET succeeds after failover" request "${new_leader}" GET validation:key

say "stopping the remaining nodes, then restarting all three with the same data directories"
stop_cluster
start_cluster_with_fresh_ports
assert_every_node_recovered_persisted_metadata
assert_snapshot_recovered

recovered_leader="$(find_leader restart 25)"
say "leader after restart: node ${recovered_leader}"
assert_reply after-failover "WAL recovery preserves the committed value" request "${recovered_leader}" GET validation:key
assert_reply OK "control value commits before deletion" request "${recovered_leader}" SET validation:control survives-final-restart
assert_reply 1 "DEL commits and reports the removed key" request "${recovered_leader}" DEL validation:key
assert_reply '(nil)' "GET observes the committed deletion" request "${recovered_leader}" GET validation:key

say "restarting all three nodes again to verify deletion durability"
stop_cluster
start_cluster_with_fresh_ports
assert_every_node_recovered_persisted_metadata

final_leader="$(find_leader final-restart 25)"
say "leader after final restart: node ${final_leader}"
assert_reply survives-final-restart "final restart preserves independent committed state" request "${final_leader}" GET validation:control
assert_reply '(nil)' "final restart preserves the committed deletion" request "${final_leader}" GET validation:key
assert_consensus_fails_without_majority "${final_leader}"

say "PASS: election, quorum safety, leader-only reads, failover, snapshot recovery, and deletion durability"
