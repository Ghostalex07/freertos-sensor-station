#!/usr/bin/env bash
# 01_keys.sh — teclas rapidas en modo linea (stdin = FIFO, sin TTY). ~6 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 10
new_case
start_bin

# 1) arranque en pipe: cabecera + ayuda (<=3 s)
if wait_for '=== Estacion de sensores FreeRTOS \(puerto POSIX\) ===' out.log 3 &&
    LC_ALL=C grep -aqF 'teclas: [t]=temp alarma' out.log; then
    ok "cabecera y ayuda en modo linea (<=3 s)"
else
    fail "cabecera y ayuda en modo linea (<=3 s)" \
        "$(tail -n 20 out.log 2>/dev/null)"
fi

# 2) stats periodicos (STATS_PERIOD_MS=5000)
if wait_for 'stats: lecturas=' out.log 8; then
    ok "stats periodicos (<=8 s)"
else
    fail "stats periodicos (<=8 s)" "$(tail -n 20 out.log 2>/dev/null)"
fi

# 3) t -> lectura de temperatura forzada (o cola llena)
send_key 't'
if wait_for 'manual: inyectada lectura de temperatura forzada|manual: cola llena' out.log 3; then
    ok "tecla t inyecta temperatura forzada"
else
    fail "tecla t inyecta temperatura forzada" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 4) h -> lectura de humedad forzada
send_key 'h'
if wait_for 'manual: inyectada lectura de humedad forzada|manual: cola llena' out.log 3; then
    ok "tecla h inyecta humedad forzada"
else
    fail "tecla h inyecta humedad forzada" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 5) p -> pausa, c -> reanudacion
send_key 'p'
p_ok=0
wait_for 'manual: sensores pausados' out.log 3 && p_ok=1
send_key 'c'
c_ok=0
wait_for 'manual: sensores reanudados' out.log 3 && c_ok=1
if [ "$p_ok" -eq 1 ] && [ "$c_ok" -eq 1 ]; then
    ok "teclas p/c pausan y reanudan"
else
    fail "teclas p/c pausan y reanudan" "p=$p_ok c=$c_ok"
fi

# 6) r -> reset de alarmas
send_key 'r'
if wait_for 'manual: contador de alarmas reiniciado' out.log 3; then
    ok "tecla r reinicia el contador de alarmas"
else
    fail "tecla r reinicia el contador de alarmas" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 7) k -> presion inversa (consumidor lento)
send_key 'k'
if wait_for 'presion inversa: consumidor lento ACTIVADO' out.log 3; then
    ok "tecla k activa el consumidor lento"
else
    fail "tecla k activa el consumidor lento" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 8) d -> sin TTY no hay dashboard
send_key 'd'
if wait_for 'dashboard no disponible sin terminal' out.log 3; then
    ok "tecla d rechaza dashboard sin terminal"
else
    fail "tecla d rechaza dashboard sin terminal" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 9) ? -> vuelve a imprimir la ayuda (>=2 ocurrencias de 'teclas:')
send_key '?'
if wait_count 'teclas:' out.log 2 3; then
    ok "tecla ? imprime la ayuda"
else
    fail "tecla ? imprime la ayuda" \
        "ocurrencias=$(LC_ALL=C grep -acE 'teclas:' out.log 2>/dev/null)"
fi

# 10) q -> salida limpia (rc=0 y 'saliendo')
send_key 'q'
sal=0
wait_for 'saliendo' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "tecla q sale con rc=0 y 'saliendo'"
else
    fail "tecla q sale con rc=0 y 'saliendo'" \
        "rc=$rc saliendo=$sal terminado=$gone" "$(tail -n 10 out.log 2>/dev/null)"
fi

finish
