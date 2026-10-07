#!/usr/bin/env bash
# 05_signals.sh — SIGINT / SIGTERM / SIGHUP: clean exit (rc=0) and no
# orphans. ~9 s.
#
# The signal is sent ONLY to the binary (the child of `timeout`), never
# to the group: if `timeout` received SIGHUP it would die with rc=129 and
# mask the real rc of the binary. The binary is located by its PPID.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 9
new_case

for sig in INT TERM HUP; do
    start_bin

    # 1) it starts and shows the banner
    if wait_for '=== Sensor station FreeRTOS \(POSIX port\) ===' out.log 3; then
        ok "$sig: starts and shows the banner"
    else
        fail "$sig: starts and shows the banner" "$(tail -n 15 out.log 2>/dev/null)"
    fi

    # Locate the binary: direct child of `timeout` (env execs, so the PID
    # is kept) whose argv contains the binary path. The comm does NOT
    # work: the POSIX port renames it to 'Scheduler'.
    binpid="$(ps -eo pid=,ppid=,args= | awk -v p="$PID" -v b="$BIN" \
        '$2 == p && index($0, b) > 0 { print $1; exit }')"
    if [ -z "$binpid" ]; then
        # Fallback: any process of the binary inside our PGID.
        for cand in $(it_live_binaries); do
            [ "$cand" = "$PID" ] && continue
            pg="$(ps -o pgid= -p "$cand" 2>/dev/null | tr -d '[:space:]')"
            if [ "$pg" = "$PGID" ]; then
                binpid="$cand"
                break
            fi
        done
    fi

    if [ -n "$binpid" ]; then
        kill "-$sig" "$binpid" 2>/dev/null || true
    else
        diag "$sig: child not found, signalling the group"
        kill "-$sig" -- "-$PGID" 2>/dev/null || true
    fi

    gone=0
    wait_gone "$PID" 5 && gone=1
    stop_bin
    rc=$?

    # 2) exits with rc=0 in <=5 s
    if [ -n "$binpid" ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
        ok "$sig: exits with rc=0 (<=5 s)"
    else
        fail "$sig: exits with rc=0 (<=5 s)" \
            "rc=$rc finished=$gone binpid=${binpid:-none}"
    fi

    # 3) exit message and zero orphans
    sal=0
    LC_ALL=C grep -aq 'exiting' out.log && sal=1
    orphans="$(it_live_binaries | tr '\n' ' ')"
    if [ "$sal" -eq 1 ] && [ -z "$orphans" ]; then
        ok "$sig: 'exiting' in output and no orphans"
    else
        fail "$sig: 'exiting' in output and no orphans" \
            "exiting=$sal orphans:${orphans:-none}"
    fi
done

finish
