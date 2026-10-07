#!/usr/bin/env bash
# 01_keys.sh — quick keys in line mode (stdin = FIFO, no TTY). ~6 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 10
new_case
start_bin

# 1) start on a pipe: banner + help (<=3 s)
if wait_for '=== Sensor station FreeRTOS \(POSIX port\) ===' out.log 3 &&
    LC_ALL=C grep -aqF 'keys: [t]=temp alarm' out.log; then
    ok "banner and help in line mode (<=3 s)"
else
    fail "banner and help in line mode (<=3 s)" \
        "$(tail -n 20 out.log 2>/dev/null)"
fi

# 2) periodic stats (STATS_PERIOD_MS=5000)
if wait_for 'stats: readings=' out.log 8; then
    ok "periodic stats (<=8 s)"
else
    fail "periodic stats (<=8 s)" "$(tail -n 20 out.log 2>/dev/null)"
fi

# 3) t -> forced temperature reading (or full queue)
send_key 't'
if wait_for 'manual: injected forced temperature reading|manual: queue full' out.log 3; then
    ok "key t injects a forced temperature reading"
else
    fail "key t injects a forced temperature reading" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 4) h -> forced humidity reading
send_key 'h'
if wait_for 'manual: injected forced humidity reading|manual: queue full' out.log 3; then
    ok "key h injects a forced humidity reading"
else
    fail "key h injects a forced humidity reading" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 5) p -> pause, c -> resume
send_key 'p'
p_ok=0
wait_for 'manual: sensors paused' out.log 3 && p_ok=1
send_key 'c'
c_ok=0
wait_for 'manual: sensors resumed' out.log 3 && c_ok=1
if [ "$p_ok" -eq 1 ] && [ "$c_ok" -eq 1 ]; then
    ok "keys p/c pause and resume"
else
    fail "keys p/c pause and resume" "p=$p_ok c=$c_ok"
fi

# 6) r -> reset the alarm counter
send_key 'r'
if wait_for 'manual: alarm counter reset' out.log 3; then
    ok "key r resets the alarm counter"
else
    fail "key r resets the alarm counter" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 7) k -> backpressure (slow consumer)
send_key 'k'
if wait_for 'backpressure: slow consumer ENABLED' out.log 3; then
    ok "key k enables the slow consumer"
else
    fail "key k enables the slow consumer" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 8) d -> no dashboard without a TTY
send_key 'd'
if wait_for 'dashboard unavailable without a terminal' out.log 3; then
    ok "key d rejects the dashboard without a terminal"
else
    fail "key d rejects the dashboard without a terminal" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 9) ? -> prints the help again (>=2 occurrences of 'keys:')
send_key '?'
if wait_count 'keys:' out.log 2 3; then
    ok "key ? prints the help"
else
    fail "key ? prints the help" \
        "occurrences=$(LC_ALL=C grep -acE 'keys:' out.log 2>/dev/null)"
fi

# 10) q -> clean exit (rc=0 and 'exiting')
send_key 'q'
sal=0
wait_for 'exiting' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "key q exits with rc=0 and 'exiting'"
else
    fail "key q exits with rc=0 and 'exiting'" \
        "rc=$rc exiting=$sal finished=$gone" "$(tail -n 10 out.log 2>/dev/null)"
fi

finish
