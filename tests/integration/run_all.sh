#!/usr/bin/env bash
# run_all.sh — driver secuencial de la suite de integracion.
#
# Uso: bash tests/integration/run_all.sh [ruta/al/binario]
# (por defecto ./build/sensor_station)
#
# Los tests NUNCA se ejecutan en paralelo: el puerto HTTP 8080 es fijo en C
# (#define HTTP_PORT 8080). Cada test tiene un reintento unico si falla, y si
# vuelve a fallar su WORKDIR se copia a $IT_LOGS para subirlo como artefacto.
set -u

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

BIN="$(readlink -f -- "${1:-./build/sensor_station}" 2>/dev/null || true)"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "::error::binario sensor_station no encontrado o no ejecutable: ${1:-./build/sensor_station}"
    exit 1
fi
export BIN

IT_LOGS="${RUNNER_TEMP:-/tmp}/it-logs"
mkdir -p "$IT_LOGS"

TESTS=(01_keys 02_demos 03_http 04_tasks_csv 05_signals 06_pty)

passed=0
failed=0
retried=""
failed_tests=""

# Solo borra WORKDIRs con el patron del propio suite.
drop_workdir() {
    local wd="$1"
    case "$wd" in
        */it-sensor-??????) rm -rf -- "$wd" ;;
    esac
}

for t in "${TESTS[@]}"; do
    out="$IT_LOGS/$t.out"
    attempt=0
    while :; do
        printf '# ===== %s (intento %d) =====\n' "$t" "$((attempt + 1))"
        timeout 40 bash "$HERE/$t.sh" "$BIN" > "$out" 2>&1
        rc=$?
        cat "$out"

        if [ "$rc" -eq 0 ]; then
            if [ "$attempt" -eq 1 ]; then
                printf '# FLAKY retry: %s paso en el segundo intento\n' "$t"
                retried="$retried $t"
            fi
            passed=$((passed + 1))
            # Limpia el WORKDIR conservado por el intento fallido previo.
            wd="$(grep -o '# workdir: .*' "$out" 2>/dev/null | head -n 1 | sed 's/^# workdir: //')"
            [ -n "$wd" ] && drop_workdir "$wd"
            break
        fi

        if [ "$attempt" -eq 0 ]; then
            printf '# FLAKY retry %s (rc=%s)\n' "$t" "$rc"
            attempt=1
            continue
        fi

        wd="$(grep -o '# workdir: .*' "$out" 2>/dev/null | head -n 1 | sed 's/^# workdir: //')"
        if [ -n "$wd" ] && [ -d "$wd" ]; then
            rm -rf -- "$IT_LOGS/$t"
            cp -r -- "$wd" "$IT_LOGS/$t"
            printf '# workdir copiado a %s\n' "$IT_LOGS/$t"
        fi
        printf '::error::test de integracion %s fallo (rc=%s); log en %s\n' "$t" "$rc" "$out"
        failed=$((failed + 1))
        failed_tests="$failed_tests $t"
        break
    done
done

printf '# ===== resumen: %d/%d OK =====\n' "$passed" "${#TESTS[@]}"
[ -n "$retried" ] && printf '# reintentados:%s\n' "$retried"
if [ "$failed" -ne 0 ]; then
    printf '# fallidos:%s\n' "$failed_tests"
    exit 1
fi
exit 0
