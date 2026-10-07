#!/usr/bin/env bash
# 02_demos.sh — demo de watchdog (w), inversion de prioridades (i) y
# prioridades con vTaskPrioritySet (v). Las demos siguen activas al salir.
# ~10 s.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 8
new_case
start_bin

if ! wait_for '=== Estacion de sensores FreeRTOS \(puerto POSIX\) ===' out.log 3; then
    fail "arranque del binario" "$(tail -n 20 out.log 2>/dev/null)"
    finish
    exit 1
fi

# 1) w -> el monitor deja de latir
send_key 'w'
if wait_for 'demo vigia: monitor detenido sin latido' out.log 3; then
    ok "tecla w detiene el latido del monitor"
else
    fail "tecla w detiene el latido del monitor" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 2) el watchdog detecta al monitor (WATCHDOG_TIMEOUT_MS=5000)
if wait_for "WATCHDOG: tarea 'monitor' sin latido" out.log 10; then
    ok "watchdog detecta al monitor sin latido (<=10 s)"
else
    fail "watchdog detecta al monitor sin latido (<=10 s)" \
        "$(tail -n 15 out.log 2>/dev/null)"
fi

# 3) w -> el monitor vuelve a latir
send_key 'w'
if wait_for 'demo vigia: monitor reanudado' out.log 3; then
    ok "tecla w reanuda el monitor"
else
    fail "tecla w reanuda el monitor" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 4) el watchdog confirma la recuperacion
if wait_for "WATCHDOG: tarea 'monitor' recupero el latido" out.log 6; then
    ok "watchdog confirma la recuperacion del latido (<=6 s)"
else
    fail "watchdog confirma la recuperacion del latido (<=6 s)" \
        "$(tail -n 15 out.log 2>/dev/null)"
fi

# 5) i -> demo de inversion de prioridades
send_key 'i'
if wait_for 'inversion: demo iniciada' out.log 3; then
    ok "tecla i inicia la demo de inversion"
else
    fail "tecla i inicia la demo de inversion" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 6) ambas fases terminan (el semaforo binario y el mutex);
#    se espera por la ultima (se imprime despues) y se verifican las dos.
if wait_for 'inversion: mutex .* la ALTA espero' out.log 6 &&
    LC_ALL=C grep -aq 'inversion: semaforo binario .* la ALTA espero' out.log; then
    ok "la demo de inversion imprime semaforo binario y mutex"
else
    fail "la demo de inversion imprime semaforo binario y mutex" \
        "$(grep -a 'inversion:' out.log 2>/dev/null | tail -n 5)"
fi

# 7) v -> prioridad del monitor bajada con vTaskPrioritySet
send_key 'v'
if wait_for 'prioridades: monitor bajado de' out.log 3; then
    ok "tecla v baja la prioridad del monitor con vTaskPrioritySet"
else
    fail "tecla v baja la prioridad del monitor con vTaskPrioritySet" \
        "$(tail -n 10 out.log 2>/dev/null)"
fi

# 8) q -> rc=0 con las demos aun activas (valida que nada cuelga)
send_key 'q'
sal=0
wait_for 'saliendo' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
    ok "tecla q sale con rc=0 con las demos activas"
else
    fail "tecla q sale con rc=0 con las demos activas" \
        "rc=$rc saliendo=$sal terminado=$gone" "$(tail -n 10 out.log 2>/dev/null)"
fi

finish
