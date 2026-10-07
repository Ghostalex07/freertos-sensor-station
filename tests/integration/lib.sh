#!/usr/bin/env bash
# lib.sh — TAP13 helpers and sensor_station process management.
# No external dependencies: bash + grep/ps/curl/awk/python3 (all on ubuntu-latest).
#
# Conventions:
#   begin N      -> prints the TAP plan (declared at the top of each test)
#   ok / fail    -> assertions; fail accepts extra diagnostic lines
#   finish       -> summary; exit 1 if there were failures and keeps the WORKDIR
#   github_error -> prints ::error:: (GitHub Actions annotation)

BIN="${BIN:-}"
WORKDIR=""
PID=""
PGID=""
FD3_OPEN=0
IT_PRE_PIDS=""

_TAP_IDX=0
_TAP_FAIL=0
_TAP_TOTAL=0

begin() {
    _TAP_IDX=0
    _TAP_FAIL=0
    _TAP_TOTAL="$1"
    printf '1..%s\n' "$1"
}

ok() {
    _TAP_IDX=$((_TAP_IDX + 1))
    printf 'ok %d - %s\n' "$_TAP_IDX" "$1"
}

fail() {
    _TAP_IDX=$((_TAP_IDX + 1))
    _TAP_FAIL=$((_TAP_FAIL + 1))
    printf 'not ok %d - %s\n' "$_TAP_IDX" "$1"
    shift
    local line
    for line in "$@"; do
        printf '# %s\n' "$line"
    done
}

github_error() { printf '::error::%s\n' "$1"; }
diag() { printf '# %s\n' "$1"; }

finish() {
    printf '# tests %d pass %d fail %d\n' \
        "$_TAP_TOTAL" "$((_TAP_TOTAL - _TAP_FAIL))" "$_TAP_FAIL"
    if [ "$_TAP_FAIL" -gt 0 ]; then
        # run_all.sh locates the WORKDIR by this mark to upload it as an artifact.
        [ -n "$WORKDIR" ] && printf '# workdir: %s\n' "$WORKDIR"
        return 1
    fi
    if [ -n "$WORKDIR" ]; then
        rm -rf -- "$WORKDIR"
        WORKDIR=""
    fi
    return 0
}

# PIDs of the processes running the binary. The comm does NOT work: the
# FreeRTOS POSIX port renames the main thread to 'Scheduler', so the
# identification is by argv. The suite's own processes are excluded
# (their argv contains 'tests/integration': run_all.sh, the test itself
# and the wrapping timeout, all with the binary path as argument).
it_live_binaries() {
    local pid args
    for pid in $(pgrep -f -- "$BIN" 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        # The PID may vanish between pgrep and the read: empty args means
        # "it no longer exists" and is NEVER counted as alive.
        args="$({ tr '\0' ' ' < "/proc/$pid/cmdline"; } 2>/dev/null)"
        [ -n "$args" ] || continue
        case "$args" in
            *tests/integration*) continue ;;
        esac
        printf '%s\n' "$pid"
    done
}

# Resolves the absolute path of the binary BEFORE cd'ing to the WORKDIR.
resolve_bin() {
    local b="${1:-${BIN:-./build/sensor_station}}"
    b="$(readlink -f -- "$b" 2>/dev/null || true)"
    if [ -z "$b" ] || [ ! -x "$b" ]; then
        github_error "sensor_station binary not found or not executable: ${1:-${BIN:-}}"
        exit 1
    fi
    BIN="$b"
}

# Creates an isolated working directory (CSVs/tasks.txt do not mix
# between tests). If the test fails the directory is NOT removed
# (debug artifact).
new_case() {
    WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/it-sensor-XXXXXX")" || {
        github_error "could not create the WORKDIR"
        exit 1
    }
    printf '# workdir: %s\n' "$WORKDIR"
    cd -- "$WORKDIR" || {
        github_error "cd to $WORKDIR failed"
        exit 1
    }
    IT_PRE_PIDS="$(it_live_binaries | tr '\n' ' ')"
    mkfifo in.fifo || {
        github_error "mkfifo failed"
        exit 1
    }
    # O_RDWR on a FIFO never blocks: avoids the reader/writer race.
    exec 3<>in.fifo
    FD3_OPEN=1
    trap '_it_cleanup' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
}

# Launches the binary in its own session (setsid) with a FIFO as input
# and a safety timeout. $! is the PID of timeout (setsid does not fork:
# the script's child is not the group leader), and its PGID is that same
# PID.
start_bin() {
    if [ ! -p in.fifo ]; then
        mkfifo in.fifo || {
            github_error "mkfifo failed"
            exit 1
        }
    fi
    if [ "$FD3_OPEN" -eq 0 ]; then
        exec 3<>in.fifo
        FD3_OPEN=1
    fi
    : > out.log
    setsid env SENSOR_STATION_SEED=42 timeout -k 2 30 "$BIN" \
        > out.log 2>&1 < in.fifo &
    PID=$!
    local i=0 pg=""
    while [ "$i" -lt 40 ]; do
        pg="$(ps -o pgid= -p "$PID" 2>/dev/null | tr -d '[:space:]')"
        [ -n "$pg" ] && break
        sleep 0.05
        i=$((i + 1))
    done
    if [ -z "$pg" ]; then
        github_error "could not get the PGID of the process (pid $PID)"
        PID=""
        exit 1
    fi
    PGID="$pg"
}

send_key() { printf '%s' "$1" >&3; }

# wait_for <regex> <file> <deadline_s> — polls every 0.15 s.
wait_for() {
    local re="$1" file="$2" deadline="${3:-5}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if [ -f "$file" ] && LC_ALL=C grep -aqE -- "$re" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_for_str <literal string> <file> <deadline_s> — for ANSI sequences.
wait_for_str() {
    local s="$1" file="$2" deadline="${3:-5}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if [ -f "$file" ] && LC_ALL=C grep -aqF -- "$s" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_count <regex> <file> <min_lines> <deadline_s>
wait_count() {
    local re="$1" file="$2" min="$3" deadline="${4:-5}"
    local steps i n
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        n="$(LC_ALL=C grep -acE -- "$re" "$file" 2>/dev/null || true)"
        if [ "${n:-0}" -ge "$min" ] 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_gone <pid> <deadline_s> — a zombie counts as finished (kill -0
# also returns 0 for zombies, that is why the state is checked in ps).
wait_gone() {
    local pid="$1" deadline="${2:-5}"
    local steps i state
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        state="$(ps -o state= -p "$pid" 2>/dev/null | tr -d '[:space:]')"
        if [ -z "$state" ] || [ "${state#Z}" != "$state" ]; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_http <url> <deadline_s>
wait_http() {
    local url="$1" deadline="${2:-8}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if curl -sf --max-time 1 -o /dev/null "$url" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# Closes fd3, terminates the group and returns the rc of the binary.
stop_bin() {
    local rc=0
    [ -n "${PID:-}" ] || return 0
    if [ "$FD3_OPEN" -eq 1 ]; then
        exec 3>&-
        FD3_OPEN=0
    fi
    kill -TERM -- "-${PGID}" 2>/dev/null || true
    if ! wait_gone "$PID" 5; then
        kill -KILL -- "-${PGID}" 2>/dev/null || true
        wait_gone "$PID" 3 || true
    fi
    wait "$PID" 2>/dev/null
    rc=$?
    PID=""
    PGID=""
    return "$rc"
}

check_sanitizers() {
    LC_ALL=C grep -qE "ERROR: |runtime error|SUMMARY: " -- "$1" 2>/dev/null
}

# Guaranteed cleanup: orphans are NEVER left between tests.
_it_cleanup() {
    local st=$?
    local p
    trap - EXIT INT TERM HUP
    if [ -n "${PID:-}" ]; then
        if kill -0 "$PID" 2>/dev/null; then
            if [ "$FD3_OPEN" -eq 1 ]; then
                exec 3>&-
                FD3_OPEN=0
            fi
            kill -TERM -- "-${PGID}" 2>/dev/null || true
            sleep 0.3
            kill -KILL -- "-${PGID}" 2>/dev/null || true
        fi
        wait "$PID" 2>/dev/null || true
    fi
    # Binaries that escaped the group (e.g. `script` creates its own
    # session for its child): only the ones that did not exist before the
    # test are killed.
    for p in $(it_live_binaries); do
        case " ${IT_PRE_PIDS:-} " in
            *" $p "*) ;;
            *) kill -TERM "$p" 2>/dev/null || true ;;
        esac
    done
    exit "${st:-0}"
}
