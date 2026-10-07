#!/usr/bin/env bash
# 05_signals.sh — SIGINT / SIGTERM / SIGHUP: salida limpia (rc=0) y sin
# huerfanos. ~9 s.
#
# La senal se manda SOLO al binario (hijo de `timeout`), nunca al grupo:
# si `timeout` recibiera SIGHUP moriria con rc=129 y enmascararia el rc real
# del binario. El binario se localiza por su PPID.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

resolve_bin "${1:-}"
begin 9
new_case

for sig in INT TERM HUP; do
    start_bin

    # 1) arranca y muestra la cabecera
    if wait_for '=== Estacion de sensores FreeRTOS \(puerto POSIX\) ===' out.log 3; then
        ok "$sig: arranca y muestra la cabecera"
    else
        fail "$sig: arranca y muestra la cabecera" "$(tail -n 15 out.log 2>/dev/null)"
    fi

    # Localiza el binario: hijo directo de `timeout` (env hace exec, asi que
    # conserva el PID) cuyo argv contenga la ruta del binario. El comm NO
    # sirve: el puerto POSIX lo renombra a 'Scheduler'.
    binpid="$(ps -eo pid=,ppid=,args= | awk -v p="$PID" -v b="$BIN" \
        '$2 == p && index($0, b) > 0 { print $1; exit }')"
    if [ -z "$binpid" ]; then
        # Fallback: cualquier proceso del binario dentro de nuestro PGID.
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
        diag "$sig: no se localizo el hijo, se anade el grupo"
        kill "-$sig" -- "-$PGID" 2>/dev/null || true
    fi

    gone=0
    wait_gone "$PID" 5 && gone=1
    stop_bin
    rc=$?

    # 2) termina con rc=0 en <=5 s
    if [ -n "$binpid" ] && [ "$gone" -eq 1 ] && [ "$rc" -eq 0 ]; then
        ok "$sig: termina con rc=0 (<=5 s)"
    else
        fail "$sig: termina con rc=0 (<=5 s)" \
            "rc=$rc terminado=$gone binpid=${binpid:-ninguno}"
    fi

    # 3) mensaje de salida y cero huerfanos
    sal=0
    LC_ALL=C grep -aq 'saliendo' out.log && sal=1
    orphans="$(it_live_binaries | tr '\n' ' ')"
    if [ "$sal" -eq 1 ] && [ -z "$orphans" ]; then
        ok "$sig: 'saliendo' en salida y sin huerfanos"
    else
        fail "$sig: 'saliendo' en salida y sin huerfanos" \
            "saliendo=$sal huerfanos:${orphans:-ninguno}"
    fi
done

finish
