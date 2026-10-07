#!/usr/bin/env bash
# 04_tasks_csv.sh — vTaskList dump to tasks.txt and the format of the
# CSVs written by the logger (readings.csv / events.csv). ~4 s.
# The CSVs are re-read after the process dies to validate the closing.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 5
new_case
start_bin

if ! wait_for '=== Sensor station FreeRTOS \(POSIX port\) ===' out.log 3; then
    fail "binary startup" "$(tail -n 20 out.log 2>/dev/null)"
    finish
    exit 1
fi

# 1) s -> task dump
send_key 's'
if wait_for 'tasks: dump written to tasks.txt \(vTaskList\)' out.log 3 &&
    [ -f tasks.txt ]; then
    ok "key s dumps tasks.txt (vTaskList)"
else
    fail "key s dumps tasks.txt (vTaskList)" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 2) tasks.txt with >=10 lines and the expected tasks
if [ -f tasks.txt ]; then
    lines="$(wc -l < tasks.txt | tr -d ' ')"
    missing=""
    for name in temp hum monitor alarm stats command logger http; do
        LC_ALL=C grep -qE "^[[:space:]]*$name[[:space:]]" tasks.txt ||
            missing="$missing $name"
    done
    if [ "$lines" -ge 10 ] && [ -z "$missing" ]; then
        ok "tasks.txt has >=10 lines and every task"
    else
        fail "tasks.txt has >=10 lines and every task" \
            "lines=$lines missing:$missing" "$(head -n 5 tasks.txt)"
    fi
else
    fail "tasks.txt has >=10 lines and every task" "tasks.txt does not exist"
fi

# 3) readings.csv: header, >=3 rows and valid fields
#    (the logger writes the sensor name: temperature/humidity, see src/main.c)
wait_count '^[0-9]+,' readings.csv 3 5 >/dev/null 2>&1 || true
readings_err="$(awk -F, '
    NR == 1 {
        if ($0 != "epoch_ms,sensor,value,sequence,alarm") {
            print "invalid header: " $0; exit 1
        }
        next
    }
    {
        if (NF != 5) { print "NF=" NF " on line " NR; exit 1 }
        if ($1 !~ /^[0-9]+$/) { print "epoch_ms not numeric: " $1; exit 1 }
        if ($2 != "temperature" && $2 != "humidity") { print "invalid sensor: " $2; exit 1 }
        if ($3 !~ /^-?[0-9]+$/) { print "value not numeric: " $3; exit 1 }
        if ($4 !~ /^[0-9]+$/) { print "sequence not numeric: " $4; exit 1 }
        if ($5 != "0" && $5 != "1") { print "invalid alarm: " $5; exit 1 }
        n++
    }
    END {
        if (n < 3) { print "only " (n + 0) " data rows"; exit 1 }
    }' readings.csv 2>&1)"
readings_rc=$?
if [ "$readings_rc" -eq 0 ]; then
    ok "readings.csv: header, >=3 rows and valid fields"
else
    fail "readings.csv: header, >=3 rows and valid fields" \
        "$readings_err" "$(head -n 5 readings.csv 2>/dev/null)"
fi

# 4) events.csv: header and >=1 event
if [ "$(head -n 1 events.csv 2>/dev/null)" = "epoch_ms,event" ] &&
    [ "$(LC_ALL=C grep -acE '^[0-9]+,"' events.csv 2>/dev/null)" -ge 1 ]; then
    ok "events.csv: header and >=1 event"
else
    fail "events.csv: header and >=1 event" \
        "$(head -n 3 events.csv 2>/dev/null)"
fi

# 5) q -> rc=0 and the CSVs re-read after the process died
send_key 'q'
sal=0
wait_for 'exiting' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ] &&
    [ -s readings.csv ] && [ -s events.csv ]; then
    ok "key q exits with rc=0 and the CSVs persist after the exit"
else
    fail "key q exits with rc=0 and the CSVs persist after the exit" \
        "rc=$rc exiting=$sal finished=$gone" \
        "readings=$(wc -l < readings.csv 2>/dev/null) events=$(wc -l < events.csv 2>/dev/null)"
fi

finish
