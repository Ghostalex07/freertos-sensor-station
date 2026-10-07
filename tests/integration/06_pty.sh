#!/usr/bin/env bash
# 06_pty.sh — dashboard ANSI via PTY real (`script -qefc` de util-linux).
# Las teclas se envian por la STDIN de `script` (FIFO) -> PTY -> binario.
# ~6 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 4
new_case

ESC="$(printf '\033')"

# `script` copia su stdin (nuestra FIFO) en el PTY maestro; el binario ve
# stdin/stdout con TTY -> arranca en dashboard. setsid aísla el grupo por si
# hay que limpiar (script crea ademas sesion propia para su hijo).
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
    github_error "no se pudo obtener el PGID de script (pid $PID)"
    exit 1
fi

# 1) el binario arranca en dashboard (borrado de pantalla \x1b[H\x1b[2J)
if wait_for_str "${ESC}[H${ESC}[2J" out.log 8 &&
    LC_ALL=C grep -aqF '=== Estacion de sensores FreeRTOS (puerto POSIX) ===' out.log; then
    ok "dashboard ANSI activo con PTY (<=8 s)"
else
    fail "dashboard ANSI activo con PTY (<=8 s)" \
        "$(head -c 600 out.log 2>/dev/null | cat -v)"
fi

# 2) la tecla t llega a traves del PTY y se refleja en el dashboard
send_key 't'
if wait_for 'manual: inyectada lectura de temperatura forzada|manual: cola llena' out.log 5; then
    ok "tecla t procesada en modo dashboard"
else
    fail "tecla t procesada en modo dashboard" \
        "$(grep -a 'ultimo evento' out.log 2>/dev/null | tail -n 3)"
fi

# 3) q -> rc=0
send_key 'q'
sal=0
wait_for 'saliendo' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "tecla q sale con rc=0 desde el dashboard"
else
    fail "tecla q sale con rc=0 desde el dashboard" \
        "rc=$rc saliendo=$sal terminado=$gone" \
        "$(tail -n 10 out.log 2>/dev/null | cat -v)"
fi

# 4) sin informes de ASan/UBSan (relevante en el job build-asan)
if check_sanitizers out.log; then
    fail "sin informes ASan/UBSan en la salida PTY" \
        "$(LC_ALL=C grep -aE 'ERROR: |runtime error|SUMMARY: ' out.log | head -n 5)"
else
    ok "sin informes ASan/UBSan en la salida PTY"
fi

finish
