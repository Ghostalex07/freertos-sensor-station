#!/usr/bin/env bash
# 02_demos.sh — watchdog demo (w), priority inversion (i) and
# priorities with vTaskPrioritySet (v). The demos are still active at
# exit. ~10 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 8
new_case
start_bin

if ! wait_for '=== Sensor station FreeRTOS \(POSIX port\) ===' out.log 3; then
    fail "binary startup" "$(tail -n 20 out.log 2>/dev/null)"
    finish
    exit 1
fi

# 1) w -> the monitor stops beating
send_key 'w'
if wait_for 'watchdog demo: monitor stopped without beating' out.log 3; then
    ok "key w stops the monitor heartbeat"
else
    fail "key w stops the monitor heartbeat" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 2) the watchdog detects the monitor (WATCHDOG_TIMEOUT_MS=5000)
if wait_for "WATCHDOG: task 'monitor' failed to beat" out.log 10; then
    ok "watchdog flags the monitor without beats (<=10 s)"
else
    fail "watchdog flags the monitor without beats (<=10 s)" \
        "$(tail -n 15 out.log 2>/dev/null)"
fi

# 3) w -> the monitor beats again
send_key 'w'
if wait_for 'watchdog demo: monitor resumed' out.log 3; then
    ok "key w resumes the monitor"
else
    fail "key w resumes the monitor" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 4) the watchdog confirms the recovery
if wait_for "WATCHDOG: task 'monitor' beat resumed" out.log 6; then
    ok "watchdog confirms the heartbeat recovery (<=6 s)"
else
    fail "watchdog confirms the heartbeat recovery (<=6 s)" \
        "$(tail -n 15 out.log 2>/dev/null)"
fi

# 5) i -> priority inversion demo
send_key 'i'
if wait_for 'inversion: demo started' out.log 3; then
    ok "key i starts the inversion demo"
else
    fail "key i starts the inversion demo" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 6) both phases finish (binary semaphore and mutex); we wait for the
#    last one (printed afterwards) and verify both.
if wait_for 'inversion: mutex .* HIGH task waited' out.log 6 &&
    LC_ALL=C grep -aq 'inversion: binary semaphore .* HIGH task waited' out.log; then
    ok "the inversion demo prints binary semaphore and mutex"
else
    fail "the inversion demo prints binary semaphore and mutex" \
        "$(grep -a 'inversion:' out.log 2>/dev/null | tail -n 5)"
fi

# 7) v -> monitor priority lowered with vTaskPrioritySet
send_key 'v'
if wait_for 'priorities: monitor lowered from' out.log 3; then
    ok "key v lowers the monitor priority with vTaskPrioritySet"
else
    fail "key v lowers the monitor priority with vTaskPrioritySet" \
        "$(tail -n 10 out.log 2>/dev/null)"
fi

# 8) q -> rc=0 with the demos still active (proves nothing hangs)
send_key 'q'
sal=0
wait_for 'exiting' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "key q exits with rc=0 with the demos active"
else
    fail "key q exits with rc=0 with the demos active" \
        "rc=$rc exiting=$sal finished=$gone" "$(tail -n 10 out.log 2>/dev/null)"
fi

finish
