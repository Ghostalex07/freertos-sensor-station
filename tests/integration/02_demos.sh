#!/usr/bin/env bash
# 02_demos.sh — watchdog demo (w), priority inversion (i), priorities
# with vTaskPrioritySet (v), ISR simulation (f) and recoverable AB/BA
# deadlock (y). The demos are still active at exit. ~25 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 13
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

# 7) f -> ISR simulation enabled from the tick hook
send_key 'f'
if wait_for 'isr: demo enabled' out.log 3; then
    ok "key f enables the ISR demo"
else
    fail "key f enables the ISR demo" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 8) the tick hook injects events (stats shows isr_events > 0)
if wait_for 'isr_events=[1-9]' out.log 8; then
    ok "the tick hook injects ISR events (isr_events > 0)"
else
    fail "the tick hook injects ISR events (isr_events > 0)" \
        "$(grep -a 'stats:' out.log 2>/dev/null | tail -n 3)"
fi

# 9) f -> ISR simulation disabled again
send_key 'f'
if wait_for 'isr: demo disabled' out.log 3; then
    ok "key f disables the ISR demo"
else
    fail "key f disables the ISR demo" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 10) y -> recoverable AB/BA deadlock demo
send_key 'y'
if wait_for 'deadlock: demo started' out.log 3; then
    ok "key y starts the deadlock demo"
else
    fail "key y starts the deadlock demo" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 11) dl1 times out (breaks the cycle) and dl2 gets both locks
if wait_for 'deadlock: dl1' out.log 6 &&
    wait_for 'deadlock: dl2' out.log 6 &&
    LC_ALL=C grep -aq 'deadlock: dl1 timed out' out.log; then
    ok "the deadlock is broken by the take timeout (dl1 recovers)"
else
    fail "the deadlock is broken by the take timeout (dl1 recovers)" \
        "$(grep -a 'deadlock:' out.log 2>/dev/null | tail -n 5)"
fi

# 12) v -> monitor priority lowered with vTaskPrioritySet
send_key 'v'
if wait_for 'priorities: monitor lowered from' out.log 3; then
    ok "key v lowers the monitor priority with vTaskPrioritySet"
else
    fail "key v lowers the monitor priority with vTaskPrioritySet" \
        "$(tail -n 10 out.log 2>/dev/null)"
fi

# 13) q -> rc=0 with the demos still active (proves nothing hangs)
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
