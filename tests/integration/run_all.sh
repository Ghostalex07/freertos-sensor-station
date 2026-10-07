#!/usr/bin/env bash
# run_all.sh — sequential driver of the integration suite.
#
# Usage: bash tests/integration/run_all.sh [path/to/binary]
# (defaults to ./build/sensor_station)
#
# The tests are NEVER run in parallel: the HTTP port 8080 is fixed in C
# (#define HTTP_PORT 8080). Every test gets a single retry if it fails, and
# if it fails again its WORKDIR is copied to $IT_LOGS to upload as an
# artifact.
set -u

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

BIN="$(readlink -f -- "${1:-./build/sensor_station}" 2>/dev/null || true)"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "::error::sensor_station binary not found or not executable: ${1:-./build/sensor_station}"
    exit 1
fi
export BIN

IT_LOGS="${RUNNER_TEMP:-/tmp}/it-logs"
mkdir -p "$IT_LOGS"

TESTS=(01_keys 02_demos 03_http 04_tasks_csv 05_signals 06_pty)

passed=0
failed=0
retried=""
failed_tests=""

# Only deletes WORKDIRs matching the suite own pattern.
drop_workdir() {
    local wd="$1"
    case "$wd" in
        */it-sensor-??????) rm -rf -- "$wd" ;;
    esac
}

for t in "${TESTS[@]}"; do
    out="$IT_LOGS/$t.out"
    attempt=0
    while :; do
        printf '# ===== %s (attempt %d) =====\n' "$t" "$((attempt + 1))"
        timeout 40 bash "$HERE/$t.sh" "$BIN" > "$out" 2>&1
        rc=$?
        cat "$out"

        if [ "$rc" -eq 0 ]; then
            if [ "$attempt" -eq 1 ]; then
                printf '# FLAKY retry: %s passed on the second attempt\n' "$t"
                retried="$retried $t"
            fi
            passed=$((passed + 1))
            # Drops the WORKDIR kept by the previous failed attempt.
            wd="$(grep -o '# workdir: .*' "$out" 2>/dev/null | head -n 1 | sed 's/^# workdir: //')"
            [ -n "$wd" ] && drop_workdir "$wd"
            break
        fi

        if [ "$attempt" -eq 0 ]; then
            printf '# FLAKY retry %s (rc=%s)\n' "$t" "$rc"
            attempt=1
            continue
        fi

        wd="$(grep -o '# workdir: .*' "$out" 2>/dev/null | head -n 1 | sed 's/^# workdir: //')"
        if [ -n "$wd" ] && [ -d "$wd" ]; then
            rm -rf -- "$IT_LOGS/$t"
            cp -r -- "$wd" "$IT_LOGS/$t"
            printf '# workdir copied to %s\n' "$IT_LOGS/$t"
        fi
        printf '::error::integration test %s failed (rc=%s); log at %s\n' "$t" "$rc" "$out"
        failed=$((failed + 1))
        failed_tests="$failed_tests $t"
        break
    done
done

printf '# ===== summary: %d/%d OK =====\n' "$passed" "${#TESTS[@]}"
[ -n "$retried" ] && printf '# retried:%s\n' "$retried"
if [ "$failed" -ne 0 ]; then
    printf '# failed:%s\n' "$failed_tests"
    exit 1
fi
exit 0
