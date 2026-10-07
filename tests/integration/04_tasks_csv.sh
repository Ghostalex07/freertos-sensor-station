#!/usr/bin/env bash
# 04_tasks_csv.sh — volcado vTaskList a tasks.txt y formato de los CSV
# generados por el logger (readings.csv / events.csv). ~4 s.
# Los CSV se releen tras la muerte del proceso para validar el cierre.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 5
new_case
start_bin

if ! wait_for '=== Estacion de sensores FreeRTOS \(puerto POSIX\) ===' out.log 3; then
    fail "arranque del binario" "$(tail -n 20 out.log 2>/dev/null)"
    finish
    exit 1
fi

# 1) s -> volcado de tareas
send_key 's'
if wait_for 'tareas: volcado en tasks.txt \(vTaskList\)' out.log 3 &&
    [ -f tasks.txt ]; then
    ok "tecla s vuelca tasks.txt (vTaskList)"
else
    fail "tecla s vuelca tasks.txt (vTaskList)" "$(tail -n 10 out.log 2>/dev/null)"
fi

# 2) tasks.txt con >=10 lineas y las tareas esperadas
if [ -f tasks.txt ]; then
    lines="$(wc -l < tasks.txt | tr -d ' ')"
    missing=""
    for name in temp hum monitor alarm stats command logger http; do
        LC_ALL=C grep -qE "^[[:space:]]*$name[[:space:]]" tasks.txt ||
            missing="$missing $name"
    done
    if [ "$lines" -ge 10 ] && [ -z "$missing" ]; then
        ok "tasks.txt tiene >=10 lineas y todas las tareas"
    else
        fail "tasks.txt tiene >=10 lineas y todas las tareas" \
            "lineas=$lines faltan:$missing" "$(head -n 5 tasks.txt)"
    fi
else
    fail "tasks.txt tiene >=10 lineas y todas las tareas" "tasks.txt no existe"
fi

# 3) readings.csv: cabecera, >=3 filas y campos validos
#    (el logger escribe el nombre del sensor: temperatura/humedad, ver main.c)
wait_count '^[0-9]+,' readings.csv 3 5 >/dev/null 2>&1 || true
readings_err="$(awk -F, '
    NR == 1 {
        if ($0 != "epoch_ms,sensor,valor,secuencia,alarma") {
            print "cabecera invalida: " $0; exit 1
        }
        next
    }
    {
        if (NF != 5) { print "NF=" NF " en linea " NR; exit 1 }
        if ($1 !~ /^[0-9]+$/) { print "epoch_ms no numerico: " $1; exit 1 }
        if ($2 != "temperatura" && $2 != "humedad") { print "sensor invalido: " $2; exit 1 }
        if ($3 !~ /^-?[0-9]+$/) { print "valor no numerico: " $3; exit 1 }
        if ($4 !~ /^[0-9]+$/) { print "secuencia no numerica: " $4; exit 1 }
        if ($5 != "0" && $5 != "1") { print "alarma invalida: " $5; exit 1 }
        n++
    }
    END {
        if (n < 3) { print "solo " (n + 0) " filas de datos"; exit 1 }
    }' readings.csv 2>&1)"
readings_rc=$?
if [ "$readings_rc" -eq 0 ]; then
    ok "readings.csv: cabecera, >=3 filas y campos validos"
else
    fail "readings.csv: cabecera, >=3 filas y campos validos" \
        "$readings_err" "$(head -n 5 readings.csv 2>/dev/null)"
fi

# 4) events.csv: cabecera y >=1 evento
if [ "$(head -n 1 events.csv 2>/dev/null)" = "epoch_ms,evento" ] &&
    [ "$(LC_ALL=C grep -acE '^[0-9]+,"' events.csv 2>/dev/null)" -ge 1 ]; then
    ok "events.csv: cabecera y >=1 evento"
else
    fail "events.csv: cabecera y >=1 evento" \
        "$(head -n 3 events.csv 2>/dev/null)"
fi

# 5) q -> rc=0 y releo de los CSV tras la muerte del proceso
send_key 'q'
sal=0
wait_for 'saliendo' out.log 5 && sal=1
gone=0
wait_gone "$PID" 5 && gone=1
stop_bin
rc=$?
if [ "$sal" -eq 1 ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ] &&
    [ -s readings.csv ] && [ -s events.csv ]; then
    ok "tecla q sale con rc=0 y los CSV persisten tras la salida"
else
    fail "tecla q sale con rc=0 y los CSV persisten tras la salida" \
        "rc=$rc saliendo=$sal terminado=$gone" \
        "readings=$(wc -l < readings.csv 2>/dev/null) events=$(wc -l < events.csv 2>/dev/null)"
fi

finish
