#!/usr/bin/env bash
# lib.sh — helpers TAP13 y gestion del proceso sensor_station.
# Sin dependencias externas: bash + grep/ps/curl/awk/python3 (todos en ubuntu-latest).
#
# Convenciones:
#   begin N      -> imprime el plan TAP (declarado al principio de cada test)
#   ok / fail    -> aserciones; fail acepta lineas de diagnostico extra
#   finish       -> resumen; exit 1 si hubo fallos y conserva el WORKDIR
#   github_error -> imprime ::error:: (anotacion de GitHub Actions)

IT_LIB_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

BIN="${BIN:-}"
WORKDIR=""
PID=""
PGID=""
FD3_OPEN=0
IT_PRE_PIDS=""

_TAP_IDX=0
_TAP_FAIL=0
_TAP_TOTAL=0

begin() {
    _TAP_IDX=0
    _TAP_FAIL=0
    _TAP_TOTAL="$1"
    printf '1..%s\n' "$1"
}

ok() {
    _TAP_IDX=$((_TAP_IDX + 1))
    printf 'ok %d - %s\n' "$_TAP_IDX" "$1"
}

fail() {
    _TAP_IDX=$((_TAP_IDX + 1))
    _TAP_FAIL=$((_TAP_FAIL + 1))
    printf 'not ok %d - %s\n' "$_TAP_IDX" "$1"
    shift
    local line
    for line in "$@"; do
        printf '# %s\n' "$line"
    done
}

github_error() { printf '::error::%s\n' "$1"; }
diag() { printf '# %s\n' "$1"; }

finish() {
    printf '# tests %d pass %d fail %d\n' \
        "$_TAP_TOTAL" "$((_TAP_TOTAL - _TAP_FAIL))" "$_TAP_FAIL"
    if [ "$_TAP_FAIL" -gt 0 ]; then
        # run_all.sh localiza el WORKDIR por esta marca para subirlo como artefacto.
        [ -n "$WORKDIR" ] && printf '# workdir: %s\n' "$WORKDIR"
        return 1
    fi
    if [ -n "$WORKDIR" ]; then
        rm -rf -- "$WORKDIR"
        WORKDIR=""
    fi
    return 0
}

# PIDs de los procesos que ejecutan el binario. El comm NO sirve: el puerto
# POSIX de FreeRTOS renombra el thread principal a 'Scheduler', asi que la
# identificacion es por argv. Se excluyen los procesos de la propia suite
# (sus argv contienen 'tests/integration': run_all.sh, el test y el timeout
# contenedor, todos con la ruta del binario como argumento).
it_live_binaries() {
    local pid args
    for pid in $(pgrep -f -- "$BIN" 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        # El PID puede desaparecer entre pgrep y la lectura: args vacio
        # significa "ya no existe" y NUNCA se cuenta como vivo.
        args="$({ tr '\0' ' ' < "/proc/$pid/cmdline"; } 2>/dev/null)"
        [ -n "$args" ] || continue
        case "$args" in
            *tests/integration*) continue ;;
        esac
        printf '%s\n' "$pid"
    done
}

# Resuelve la ruta absoluta del binario ANTES de hacer cd al WORKDIR.
resolve_bin() {
    local b="${1:-${BIN:-./build/sensor_station}}"
    b="$(readlink -f -- "$b" 2>/dev/null || true)"
    if [ -z "$b" ] || [ ! -x "$b" ]; then
        github_error "binario sensor_station no encontrado o no ejecutable: ${1:-${BIN:-}}"
        exit 1
    fi
    BIN="$b"
}

# Crea un directorio de trabajo aislado (CSVs/tasks.txt no se mezclan entre tests).
# Si el test falla el directorio NO se borra (artefacto de debug).
new_case() {
    WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/it-sensor-XXXXXX")" || {
        github_error "no se pudo crear el WORKDIR"
        exit 1
    }
    printf '# workdir: %s\n' "$WORKDIR"
    cd -- "$WORKDIR" || {
        github_error "cd a $WORKDIR fallo"
        exit 1
    }
    IT_PRE_PIDS="$(it_live_binaries | tr '\n' ' ')"
    mkfifo in.fifo || {
        github_error "mkfifo fallo"
        exit 1
    }
    # O_RDWR sobre FIFO nunca bloquea: evita la carrera lector/escritor.
    exec 3<>in.fifo
    FD3_OPEN=1
    trap '_it_cleanup' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
}

# Lanza el binario en sesion propia (setsid) con FIFO de entrada y timeout de
# seguridad. $! es el PID de timeout (setsid no hace fork: el hijo del script
# no es lider de grupo), y su PGID es el mismo PID.
start_bin() {
    if [ ! -p in.fifo ]; then
        mkfifo in.fifo || {
            github_error "mkfifo fallo"
            exit 1
        }
    fi
    if [ "$FD3_OPEN" -eq 0 ]; then
        exec 3<>in.fifo
        FD3_OPEN=1
    fi
    : > out.log
    setsid env SENSOR_STATION_SEED=42 timeout -k 2 30 "$BIN" \
        > out.log 2>&1 < in.fifo &
    PID=$!
    local i=0 pg=""
    while [ "$i" -lt 40 ]; do
        pg="$(ps -o pgid= -p "$PID" 2>/dev/null | tr -d '[:space:]')"
        [ -n "$pg" ] && break
        sleep 0.05
        i=$((i + 1))
    done
    if [ -z "$pg" ]; then
        github_error "no se pudo obtener el PGID del proceso (pid $PID)"
        PID=""
        exit 1
    fi
    PGID="$pg"
}

send_key() { printf '%s' "$1" >&3; }

# wait_for <regex> <fichero> <deadline_s> — poll cada 0,15 s.
wait_for() {
    local re="$1" file="$2" deadline="${3:-5}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if [ -f "$file" ] && LC_ALL=C grep -aqE -- "$re" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_for_str <cadena literal> <fichero> <deadline_s> — para secuencias ANSI.
wait_for_str() {
    local s="$1" file="$2" deadline="${3:-5}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if [ -f "$file" ] && LC_ALL=C grep -aqF -- "$s" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_count <regex> <fichero> <min_lineas> <deadline_s>
wait_count() {
    local re="$1" file="$2" min="$3" deadline="${4:-5}"
    local steps i n
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        n="$(LC_ALL=C grep -acE -- "$re" "$file" 2>/dev/null || true)"
        if [ "${n:-0}" -ge "$min" ] 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_gone <pid> <deadline_s> — un zombie cuenta como terminado (kill -0
# devuelve 0 tambien con zombies, por eso se consulta el estado en ps).
wait_gone() {
    local pid="$1" deadline="${2:-5}"
    local steps i state
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        state="$(ps -o state= -p "$pid" 2>/dev/null | tr -d '[:space:]')"
        if [ -z "$state" ] || [ "${state#Z}" != "$state" ]; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# wait_http <url> <deadline_s>
wait_http() {
    local url="$1" deadline="${2:-8}"
    local steps i
    steps="$(awk -v d="$deadline" 'BEGIN { print int(d / 0.15) + 1 }')"
    i=0
    while [ "$i" -lt "$steps" ]; do
        if curl -sf --max-time 1 -o /dev/null "$url" 2>/dev/null; then
            return 0
        fi
        sleep 0.15
        i=$((i + 1))
    done
    return 1
}

# Cierra fd3, termina el grupo y devuelve el rc del binario.
stop_bin() {
    local rc=0
    [ -n "${PID:-}" ] || return 0
    if [ "$FD3_OPEN" -eq 1 ]; then
        exec 3>&-
        FD3_OPEN=0
    fi
    kill -TERM -- "-${PGID}" 2>/dev/null || true
    if ! wait_gone "$PID" 5; then
        kill -KILL -- "-${PGID}" 2>/dev/null || true
        wait_gone "$PID" 3 || true
    fi
    wait "$PID" 2>/dev/null
    rc=$?
    PID=""
    PGID=""
    return "$rc"
}

check_sanitizers() {
    LC_ALL=C grep -qE "ERROR: |runtime error|SUMMARY: " -- "$1" 2>/dev/null
}

# Limpieza garantizada: NUNCA se dejan huerfanos entre tests.
_it_cleanup() {
    local st=$?
    local p
    trap - EXIT INT TERM HUP
    if [ -n "${PID:-}" ]; then
        if kill -0 "$PID" 2>/dev/null; then
            if [ "$FD3_OPEN" -eq 1 ]; then
                exec 3>&-
                FD3_OPEN=0
            fi
            kill -TERM -- "-${PGID}" 2>/dev/null || true
            sleep 0.3
            kill -KILL -- "-${PGID}" 2>/dev/null || true
        fi
        wait "$PID" 2>/dev/null || true
    fi
    # Binarios que se hayan escapado del grupo (p. ej. `script` crea sesion
    # propia para su hijo): se matan solo los que no existian antes del test.
    for p in $(it_live_binaries); do
        case " ${IT_PRE_PIDS:-} " in
            *" $p "*) ;;
            *) kill -TERM "$p" 2>/dev/null || true ;;
        esac
    done
    exit "${st:-0}"
}
