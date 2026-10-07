#!/usr/bin/env bash
# 03_http.sh — servidor HTTP en 127.0.0.1:8080: /metrics (JSON + schema) y
# pagina HTML por defecto. ~6 s. El puerto es fijo: nunca corre en paralelo.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Valida el schema de /metrics. rc=0 ok, rc=2 (reintentable: aun sin lecturas),
# rc=1 error de schema.
schema_check() {
    python3 - "$1" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "rb") as fh:
        data = fh.read()
    doc = json.loads(data)
except Exception as exc:  # noqa: BLE001 - se reintenta: cuerpo posible truncado
    print("JSON ilegible: %s" % exc)
    sys.exit(2)

expected = {
    "uptime_s", "estado", "alarmas", "lecturas", "perdidas", "picos",
    "log_perdidas", "cola", "heap_libre", "watchdog", "watchdog_fallos",
    "cpu_ocupado_pct", "cpu_pct", "temp", "hum", "ultimo_evento",
}
keys = set(doc)
if keys != expected:
    print("claves distintas: faltan=%s sobran=%s"
          % (sorted(expected - keys), sorted(keys - expected)))
    sys.exit(1)

cpu_keys = set(doc["cpu_pct"])
if cpu_keys != {"temp", "hum", "monitor", "alarm"}:
    print("cpu_pct claves invalidas: %s" % sorted(cpu_keys))
    sys.exit(1)

if doc["estado"] not in ("normal", "pausa", "alarma"):
    print("estado invalido: %r" % (doc["estado"],))
    sys.exit(1)
if doc["watchdog"] not in ("ok", "alerta"):
    print("watchdog invalido: %r" % (doc["watchdog"],))
    sys.exit(1)

for key in ("uptime_s", "alarmas", "lecturas", "perdidas", "picos",
            "log_perdidas", "cola", "watchdog_fallos"):
    val = doc[key]
    if isinstance(val, bool) or not isinstance(val, int) or val < 0:
        print("%s invalido: %r" % (key, val))
        sys.exit(1)

if isinstance(doc["heap_libre"], bool) or not isinstance(doc["heap_libre"], int) \
        or doc["heap_libre"] <= 0:
    print("heap_libre invalido: %r" % (doc["heap_libre"],))
    sys.exit(1)

def numeric(val, key, strictly_positive=False):
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        print("%s no numerico: %r" % (key, val))
        sys.exit(1)
    if strictly_positive and not val > 0:
        print("%s no positivo: %r" % (key, val))
        sys.exit(1)
    if val < 0:
        print("%s negativo: %r" % (key, val))
        sys.exit(1)

numeric(doc["cpu_ocupado_pct"], "cpu_ocupado_pct")
for key, val in doc["cpu_pct"].items():
    numeric(val, "cpu_pct." + key)
numeric(doc["temp"], "temp")
numeric(doc["hum"], "hum")

if doc["lecturas"] < 1:
    sys.exit(2)

if not isinstance(doc["ultimo_evento"], str) or not doc["ultimo_evento"].strip():
    print("ultimo_evento vacio: %r" % (doc["ultimo_evento"],))
    sys.exit(1)
PY
}

resolve_bin "${1:-}"
begin 5
new_case
start_bin

# 1) el servidor HTTP responde (<=8 s)
if wait_http 'http://127.0.0.1:8080/metrics' 8; then
    ok "GET /metrics responde (<=8 s)"
else
    fail "GET /metrics responde (<=8 s)" "$(tail -n 20 out.log 2>/dev/null)"
fi

# 2) 200 + Content-Type JSON
curl -s -D headers.txt -o metrics.json --max-time 3 \
    'http://127.0.0.1:8080/metrics' || true
code="$(head -n 1 headers.txt 2>/dev/null | awk '{print $2}')"
if [ "$code" = "200" ] &&
    LC_ALL=C grep -qi '^Content-Type:.*json' headers.txt 2>/dev/null; then
    ok "GET /metrics -> 200 con Content-Type json"
else
    fail "GET /metrics -> 200 con Content-Type json" \
        "code=$code" "$(head -n 5 headers.txt 2>/dev/null)"
fi

# 3) schema JSON estricto (con reintentos hasta que haya >=1 lectura)
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
    ok "schema JSON de /metrics estricto y coherente"
else
    fail "schema JSON de /metrics estricto y coherente" "$err"
fi

# 4) pagina HTML por defecto
code="$(curl -s -o index.html -w '%{http_code}' --max-time 3 \
    'http://127.0.0.1:8080/' || echo 000)"
if [ "$code" = "200" ] &&
    LC_ALL=C grep -qF '<h1>Estacion de sensores FreeRTOS</h1>' index.html &&
    LC_ALL=C grep -qF '<a href="/metrics">' index.html; then
    ok "GET / -> 200 HTML con enlace a /metrics"
else
    fail "GET / -> 200 HTML con enlace a /metrics" \
        "code=$code" "$(head -n 5 index.html 2>/dev/null)"
fi

# 5) q -> rc=0
send_key 'q'
sal=0
wait_for 'saliendo' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "tecla q sale con rc=0"
else
    fail "tecla q sale con rc=0" "rc=$rc saliendo=$sal terminado=$gone"
fi

finish
