#!/usr/bin/env bash
# 03_http.sh — HTTP server on 127.0.0.1:8080: /metrics (JSON + schema)
# and the default HTML page. ~6 s. The port is fixed: never in parallel.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Validates the /metrics schema. rc=0 ok, rc=2 (retryable: no readings
# yet), rc=1 schema error.
schema_check() {
    python3 - "$1" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "rb") as fh:
        data = fh.read()
    doc = json.loads(data)
except Exception as exc:  # noqa: BLE001 - retried: body may be truncated
    print("unreadable JSON: %s" % exc)
    sys.exit(2)

expected = {
    "uptime_s", "state", "alarms", "readings", "dropped", "spikes",
    "log_dropped", "queue", "heap_free", "watchdog", "watchdog_fails",
    "cpu_busy_pct", "cpu_pct", "temp", "hum", "last_event",
}
keys = set(doc)
if keys != expected:
    print("different keys: missing=%s extra=%s"
          % (sorted(expected - keys), sorted(keys - expected)))
    sys.exit(1)

cpu_keys = set(doc["cpu_pct"])
if cpu_keys != {"temp", "hum", "monitor", "alarm"}:
    print("invalid cpu_pct keys: %s" % sorted(cpu_keys))
    sys.exit(1)

if doc["state"] not in ("normal", "paused", "alarm"):
    print("invalid state: %r" % (doc["state"],))
    sys.exit(1)
if doc["watchdog"] not in ("ok", "alert"):
    print("invalid watchdog: %r" % (doc["watchdog"],))
    sys.exit(1)

for key in ("uptime_s", "alarms", "readings", "dropped", "spikes",
            "log_dropped", "queue", "watchdog_fails"):
    val = doc[key]
    if isinstance(val, bool) or not isinstance(val, int) or val < 0:
        print("invalid %s: %r" % (key, val))
        sys.exit(1)

if isinstance(doc["heap_free"], bool) or not isinstance(doc["heap_free"], int) \
        or doc["heap_free"] <= 0:
    print("invalid heap_free: %r" % (doc["heap_free"],))
    sys.exit(1)

def numeric(val, key, strictly_positive=False):
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        print("%s not numeric: %r" % (key, val))
        sys.exit(1)
    if strictly_positive and not val > 0:
        print("%s not positive: %r" % (key, val))
        sys.exit(1)
    if val < 0:
        print("%s negative: %r" % (key, val))
        sys.exit(1)

numeric(doc["cpu_busy_pct"], "cpu_busy_pct")
for key, val in doc["cpu_pct"].items():
    numeric(val, "cpu_pct." + key)
numeric(doc["temp"], "temp")
numeric(doc["hum"], "hum")

if doc["readings"] < 1:
    sys.exit(2)

if not isinstance(doc["last_event"], str) or not doc["last_event"].strip():
    print("empty last_event: %r" % (doc["last_event"],))
    sys.exit(1)
PY
}

resolve_bin "${1:-}"
begin 5
new_case
start_bin

# 1) the HTTP server answers (<=8 s)
if wait_http 'http://127.0.0.1:8080/metrics' 8; then
    ok "GET /metrics answers (<=8 s)"
else
    fail "GET /metrics answers (<=8 s)" "$(tail -n 20 out.log 2>/dev/null)"
fi

# 2) 200 + JSON Content-Type
curl -s -D headers.txt -o metrics.json --max-time 3 \
    'http://127.0.0.1:8080/metrics' || true
code="$(head -n 1 headers.txt 2>/dev/null | awk '{print $2}')"
if [ "$code" = "200" ] &&
    LC_ALL=C grep -qi '^Content-Type:.*json' headers.txt 2>/dev/null; then
    ok "GET /metrics -> 200 with JSON Content-Type"
else
    fail "GET /metrics -> 200 with JSON Content-Type" \
        "code=$code" "$(head -n 5 headers.txt 2>/dev/null)"
fi

# 3) strict JSON schema (retried until there is >=1 reading)
err=""
schema_ok=0
deadline=$(($(date +%s) + 5))
while :; do
    curl -sf --max-time 2 -o metrics.json \
        'http://127.0.0.1:8080/metrics' 2>/dev/null || true
    err="$(schema_check metrics.json 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        schema_ok=1
        break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 0.3
done
if [ "$schema_ok" -eq 1 ]; then
    ok "strict and coherent JSON schema for /metrics"
else
    fail "strict and coherent JSON schema for /metrics" "$err"
fi

# 4) default HTML page
code="$(curl -s -o index.html -w '%{http_code}' --max-time 3 \
    'http://127.0.0.1:8080/' || echo 000)"
if [ "$code" = "200" ] &&
    LC_ALL=C grep -qF '<h1>Sensor station FreeRTOS</h1>' index.html &&
    LC_ALL=C grep -qF '<a href="/metrics">' index.html; then
    ok "GET / -> 200 HTML with a link to /metrics"
else
    fail "GET / -> 200 HTML with a link to /metrics" \
        "code=$code" "$(head -n 5 index.html 2>/dev/null)"
fi

# 5) q -> rc=0
send_key 'q'
sal=0
wait_for 'exiting' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "key q exits with rc=0"
else
    fail "key q exits with rc=0" "rc=$rc exiting=$sal finished=$gone"
fi

finish
