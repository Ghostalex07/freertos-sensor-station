#!/usr/bin/env bash
# 06_pty.sh — ANSI dashboard over a real PTY (`script -qefc` from
# util-linux). The keys are sent through the stdin of `script` (FIFO)
# -> PTY -> binary. ~6 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 4
new_case

ESC="$(printf '\033')"

# `script` copies its stdin (our FIFO) to the PTY master; the binary sees
# stdin/stdout with a TTY -> starts in dashboard mode. setsid isolates the
# group in case cleanup is needed (script also creates its own session
# for its child).
: > out.log
setsid env SENSOR_STATION_SEED=42 timeout -k 2 30 \
    script -qefc "$BIN" typescript.log < in.fifo > out.log 2>&1 &
PID=$!
i=0
PGID=""
while [ "$i" -lt 40 ]; do
    PGID="$(ps -o pgid= -p "$PID" 2>/dev/null | tr -d '[:space:]')"
    [ -n "$PGID" ] && break
    sleep 0.05
    i=$((i + 1))
done
if [ -z "$PGID" ]; then
    github_error "could not get the PGID of script (pid $PID)"
    exit 1
fi

# 1) the binary starts in dashboard (screen clear \x1b[H\x1b[2J)
if wait_for_str "${ESC}[H${ESC}[2J" out.log 8 &&
    LC_ALL=C grep -aqF '=== Sensor station FreeRTOS (POSIX port) ===' out.log; then
    ok "ANSI dashboard active with a PTY (<=8 s)"
else
    fail "ANSI dashboard active with a PTY (<=8 s)" \
        "$(head -c 600 out.log 2>/dev/null | cat -v)"
fi

# 2) the t key reaches the binary through the PTY and shows on the dashboard
send_key 't'
if wait_for 'manual: injected forced temperature reading|manual: queue full' out.log 5; then
    ok "key t processed in dashboard mode"
else
    fail "key t processed in dashboard mode" \
        "$(grep -a 'last event' out.log 2>/dev/null | tail -n 3)"
fi

# 3) q -> rc=0
send_key 'q'
sal=0
wait_for 'exiting' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "key q exits with rc=0 from the dashboard"
else
    fail "key q exits with rc=0 from the dashboard" \
        "rc=$rc exiting=$sal finished=$gone" \
        "$(tail -n 10 out.log 2>/dev/null | cat -v)"
fi

# 4) no ASan/UBSan reports (relevant on the build-asan job)
if check_sanitizers out.log; then
    fail "no ASan/UBSan reports in the PTY output" \
        "$(LC_ALL=C grep -aE 'ERROR: |runtime error|SUMMARY: ' out.log | head -n 5)"
else
    ok "no ASan/UBSan reports in the PTY output"
fi

finish
