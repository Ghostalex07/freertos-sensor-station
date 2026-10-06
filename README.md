# Estación de sensores con FreeRTOS

[![CI](https://github.com/Ghostalex07/freertos-sensor-station/actions/workflows/ci.yml/badge.svg)](https://github.com/Ghostalex07/freertos-sensor-station/actions/workflows/ci.yml)

Demo educativa de **FreeRTOS** que simula una estación de sensores de temperatura y humedad, corriendo en **Linux** sobre el puerto `GCC_POSIX` (las tareas FreeRTOS se ejecutan como hilos `pthread`, sin necesidad de placa ni emulador).

![Demo con dashboard, alarma y sparklines](docs/demo.gif)

## Características destacadas

- 🐧 **Sin hardware**: usa el puerto `GCC_POSIX` de FreeRTOS, así que puedes probar multitarea real en tu propio PC.
- 📡 **Pipeline clásico de RTOS**: sensores → cola → monitor → semáforo → tarea de alarma → event group.
- ⏱️ **Periodicidad garantizada** con `vTaskDelayUntil` (no se acumula deriva).
- ⌨️ **Interfaz interactiva** por stdin en modo terminal raw: inyectar picos, pausar sensores, reiniciar contadores.
- 🖥️ **Dashboard interactivo** en terminal (tecla `d`): barras de temperatura/humedad/cola/heap, **CPU % por tarea** (`uxTaskGetSystemState` + run-time stats a 1 µs) y **sparklines** con las últimas 32 lecturas.
- 📊 **Telemetría propia**: heap libre, nº de tareas, mínimo de pila y ocupación de CPU cada 5 s.
- 📝 **Logging CSV**: la tarea `logger` escribe `readings.csv` (cada lectura) y `events.csv` (alarmas, watchdog, eventos) a través de un **stream buffer** y un **message buffer**.
- 🐕 **Watchdog por software**: vigila el latido de las 7 tareas; si una supera su timeout lo reporta como evento (pásala con `w` para verlo en acción).
- 🌐 **Endpoint HTTP**: servidor JSON mínimo en `127.0.0.1:8080/metrics` con estado, CPU %, heap, watchdog y última lectura (`curl` para verlo).
- 🛑 **Salida limpia con señales**: `SIGINT`, `SIGTERM` y `SIGHUP` terminan la demo ordenadamente (también con stdin cerrado).
- 🔀 **Stream/message buffers**: binario para lecturas (stream) y texto para eventos (message buffer), ambos primitivas reales de FreeRTOS.
- 🧰 **Primitivas avanzadas con demo interactiva**: cola de colas (*queue set*) + `xQueueOverwrite`/`xQueuePeek`, sincronización temp+hum con event group, inversión de prioridades semáforo vs mutex (tecla `i`), `vTaskPrioritySet` (tecla `v`), backpressure con consumidor lento (tecla `k`) y volcado `vTaskList` (tecla `s`).
- 🖼️ **Página HTML** en `http://127.0.0.1:8080/` (además del JSON `/metrics`, ahora con **CPU % por tarea**).
- 📜 **Rotación de CSV**: `readings.csv` rota a `readings.1.csv` al superar 64 KiB.
- 🧪 **Tests unitarios**: funciones puras en `logic.c` con `ctest` (`tests/test_logic.c`).
- ⚙️ **Alojamiento estático**: la tarea `logger` usa `xTaskCreateStatic` con TCB y pila propios (sin `pvPortMalloc`).
- 🎲 **Semilla reproducible**: `SENSOR_STATION_SEED=42 ./build/sensor_station` genera lecturas deterministas (útil en CI).
- 🔧 **Configuración centralizada** en `FreeRTOSConfig.h` y en la cabecera de `main.c`.
- 🏗️ **Build con CMake**: descarga el kernel con `FetchContent` o usa uno local con `-DFREERTOS_KERNEL_PATH`.

---

## Requisitos

| Requisito | Versión mínima |
| --- | --- |
| Sistema operativo | Linux |
| Compilador | `gcc` |
| CMake | 3.15 |

> [!NOTE]
> En la **primera configuración**, CMake descarga el kernel FreeRTOS desde GitHub con `FetchContent` (FreeRTOS-Kernel **V11.3.1**), así que necesitas conexión a red. Si trabajas **sin red**, clona el kernel por tu cuenta y pásale su ruta con `-DFREERTOS_KERNEL_PATH=/ruta/al/FreeRTOS-Kernel`.

---

## Compilar y ejecutar

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # tests unitarios (opcional)
./build/sensor_station
```

### Variante sin conexión (kernel local)

```bash
cmake -S . -B build -DFREERTOS_KERNEL_PATH=/ruta/al/FreeRTOS-Kernel
cmake --build build -j
./build/sensor_station
```

El ejecutable se genera como `sensor_station` dentro de `build/`.

---

## Controles

Pulsa las teclas mientras la aplicación está en ejecución. Si stdout es una **terminal**, la demo arranca en modo *dashboard* (pantalla completa con barras); con stdout redirigido a fichero/pipe usa modo línea. La tecla `d` alterna entre ambos.

| Tecla | Acción |
| :---: | --- |
| `t` | Inyecta una lectura de temperatura fuera de umbral (dispara alarma) |
| `h` | Inyecta una lectura de humedad fuera de umbral (dispara alarma) |
| `p` | Pausa los sensores (bandera `xSensorsPaused`, sin `vTaskSuspend`) |
| `c` | Reanuda los sensores |
| `r` | Reinicia el contador de alarmas |
| `d` | Alterna entre modo dashboard y modo línea (solo con terminal) |
| `w` | Telemetría de watchdog: suspende/reanuda `monitor` para ver la detección de «sin latido» y la recuperación |
| `i` | **Inversión de prioridades**: tarea BAJA toma el recurso (semáforo binario 1ª fase, mutex con herencia 2ª fase); la ALTA espera y se imprime su tiempo de bloqueo |
| `v` | **`vTaskPrioritySet`**: baja `monitor` de prioridad 3 a 1 durante 5 s y la restaura |
| `k` | **Backpressure**: activa/desactiva un consumidor lento (2 s por lectura) y observa cómo sube `perdidas` |
| `s` | **`vTaskList`**: vuelca el estado de todas las tareas a `tasks.txt` (y lo imprime en modo línea) |
| `q` | Sale limpia de la aplicación |
| `?` | Muestra la ayuda |

---

## Arquitectura

### Flujo de datos

```
 ┌────────────────┐   ┌────────────────┐
 │  tarea temp    │   │  tarea hum     │      prioridad 2
 │  (700 ms)      │   │  (1100 ms)     │      vTaskDelayUntil
 └───────┬────────┘   └───────┬────────┘
         │  xQueueSend        │  xQueueSend
         └──────────┬─────────┘
                    ▼
        ┌───────────────────────┐
        │   cola (8 elementos)  │
        └───────────┬───────────┘
                    │  xQueueReceive (bloqueante)
                    ▼
        ┌───────────────────────┐
        │   tarea monitor       │  prioridad 3
        │  imprime lecturas     │
        │  detecta alarmas      │
        └───────────┬───────────┘
                    │  xSemaphoreGive  (¿alarma?)
                    ▼
        ┌───────────────────────┐
        │   tarea alarm         │  prioridad 4 (máxima)
        │  xSemaphoreTake       │
        │  contador + mutex     │
        │  imprime alarma       │
        └───────────┬───────────┘
                    │  xEventGroupSetBits
                    ▼
        ┌───────────────────────┐
        │     event group       │   bit de alarma activa
        │  (se limpia a los 3 s)│
        └───────────────────────┘

  ── Independientes ──────────────────────────────────────────
  ┌────────────────┐        ┌──────────────────────────────┐
  │  tarea stats   │        │  timer de software (10 s)    │
  │  prioridad 1   │        │  xTimerCreate                │
  │  cada 5 s      │        │  inyecta picos de temperatura│
  │  (o 500 ms     │        └──────────────────────────────┘
  │   dashboard)   │        CPU% por tarea: uxTaskGetSystemState
  └────────────────┘
  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
  │  logger (CSV)  │  │  watchdog      │  │  http (8080)   │
  │  stream+msgbuf │  │  latido x7     │  │  JSON metrics  │
  └────────────────┘  └────────────────┘  └────────────────┘
```

### Primitivas de FreeRTOS utilizadas

| Primitiva | APIs | Papel en la demo |
| --- | --- | --- |
| **Cola** | `xQueueCreate`, `xQueueSend`, `xQueueReceive` | Canaliza las lecturas de los sensores hacia el monitor (8 posiciones). |
| **Mutex** | `xSemaphoreCreateMutex`, `xSemaphoreTake`, `xSemaphoreGive` | Uno protege la salida compartida de `printf`, otro el contador de alarmas. |
| **Semáforo binario** | `xSemaphoreCreateBinary`, `xSemaphoreTake`, `xSemaphoreGive` | El monitor avisa a la tarea de alarma cuando detecta un umbral superado. |
| **Event group** | `xEventGroupCreate`, `xEventGroupSetBits`, `xEventGroupClearBits` | Marca que hay una alarma activa y la limpia pasados 3 s. |
| **Semáforo contador** | `xSemaphoreCreateCounting` | Cuenta las lecturas descartadas cuando la cola está llena; `stats` las consume. |
| **Notificación de tareas** | `xTaskNotifyGive`, `xTaskNotifyWait` | Despierta al instante a la tarea `stats` para redibujar el dashboard. |
| **Timer de software** | `xTimerCreate` | Cada 10 s inyecta un pico de temperatura que provoca alarma. |
| **Gestión de tareas** | `xTaskCreate`, `xTaskCreateStatic`, bandera `xSensorsPaused` | Crea las 15 tareas (una de ellas con alojamiento estático); `p`/`c` pausan los sensores con una bandera (sin bloquear tareas). |
| **Periodicidad** | `vTaskDelayUntil` | Los sensores se ejecutan cada 700 ms / 1100 ms sin deriva. |
| **Diagnóstico** | `xPortGetFreeHeapSize`, `uxTaskGetStackHighWaterMark`, `uxTaskGetSystemState` | La tarea `stats` reporta heap, pila mínima y CPU % por tarea (*run-time stats*). |
| **Stream buffer** | `xStreamBufferCreate`, `xStreamBufferSend`, `xStreamBufferReceive` | Binario de lecturas sensor→logger; el monitor envía cada muestra y `logger` vuelca `readings.csv`. |
| **Message buffer** | `xMessageBufferCreate`, `xMessageBufferSend`, `xMessageBufferReceive` | Texto de eventos (alarmas, watchdog, http…) hacia `events.csv`. |
| **Cola de colas** | `xQueueCreateSet`, `xQueueAddToSet`, `xQueueSelectFromSet` | El conjunto recoge las colas «último valor» de temp y hum; `aggreg` espera con la cola de colas y resuelve con `xQueuePeek`. |
| **`xQueueOverwrite`** | `xQueueOverwrite` | Colas de longitud 1 con el valor más reciente de cada sensor (patrón «latest value», sin perder muestras). |
| **`xQueuePeek`** | `xQueuePeek` | `aggreg` lee el último valor **sin consumirlo**, para poder calcular la media de ambas series. |
| **Sincronización (event group)** | `xEventGroupWaitBits` (espera de todos los bits) | `sync` solo imprime cuando llegan temp **y** hum del mismo ciclo (bits separados, espera con `xWaitForAllBits`). |
| **Inversión de prioridades** | semáforo binario vs `xSemaphoreCreateMutex` | Demo `i`: la tarea BAJA retiene el recurso mientras la ALTA espera; el mutex aplica herencia y reduce la espera de ~1 s a <10 ms. |
| **`vTaskPrioritySet`** | `vTaskPrioritySet` | Demo `v`: baja y restaura la prioridad de `monitor` en caliente. |
| **Backpressure** | consumidor bloqueante + `xSemaphoreGive` (contador) | Demo `k`: con la cola llena, los descartes crecen y se cuentan en `perdidas`. |
| **Alojamiento estático** | `xTaskCreateStatic` | La tarea `logger` usa TCB y pila estáticos (sin `pvPortMalloc`). |
| **`vTaskList`** | `vTaskList` (trace facility) | Demo `s`: vuelca nombre/estado/prioridad/pila de todas las tareas a `tasks.txt`. |
| **Sockets POSIX** | `socket`, `accept`, `send` | La tarea `http` expone `127.0.0.1:8080` (JSON y HTML) fuera del kernel, estilo LWIP. |

---

## Observabilidad

### CSV (`logger`)

| Fichero | Contenido |
| --- | --- |
| `readings.csv` | `epoch_ms,sensor,valor,secuencia,alarma` — cada lectura que pasa por el monitor. |
| `events.csv` | `epoch_ms,evento` — alarmas, watchdog, eventos HTTP, arranque y apagado. |

Al superar los **64 KiB**, `readings.csv` rota a `readings.1.csv` y se reabre con cabecera. Para graficarlo sin dependencias:

```bash
python3 tools/plot_csv.py readings.csv docs/chart.svg   # genera la serie temporal
```

![Lecturas de temperatura y humedad](docs/chart.svg)

### HTTP (`http`)

```bash
curl http://127.0.0.1:8080/metrics
# {"uptime_s":2,"estado":"normal","alarmas":0,...,"cpu_pct":{"temp":0.4,...},...}

curl http://127.0.0.1:8080/          # página HTML con auto-refresh cada 2 s
```

El JSON incluye ahora **`cpu_pct` con el uso de CPU (%) de temp, hum, monitor y alarm**. Se cierra solo al recibir la respuesta (sin `keep-alive`) y usa `MSG_DONTWAIT` para no bloquear el planificador.

### Watchdog

Cada tarea latida con `vWatchdogBeat()`; la tarea `watchdog` compara con timeouts propios (5 s, 8 s para `stats`). Pasa `w` para suspender `monitor` y ver el evento `sin latido` + `recupero el latido`.

### Semilla reproducible

```bash
SENSOR_STATION_SEED=42 ./build/sensor_station   # lecturas idénticas en cada run
```

---

## Configuración

Todo lo ajustable está en dos sitios:

| Qué | Dónde | Detalle |
| --- | --- | --- |
| Umbrales de alarma | `main.c` | Constantes `TEMP_ALARM_THRESHOLD` y `HUM_ALARM_THRESHOLD` al inicio del archivo. |
| Periodo de los sensores | `main.c` | Tareas `xTempSensor` y `xHumSensor` (700 ms y 1100 ms). |
| Tamaño del heap | `FreeRTOSConfig.h` | `configTOTAL_HEAP_SIZE = 524288` (512 KiB). |
| Tamaño de pila mínimo | `FreeRTOSConfig.h` | `configMINIMAL_STACK_SIZE = 2048`. |
| Frecuencia del tick | `FreeRTOSConfig.h` | 100 Hz (10 ms por tick). |
| Detección de desbordamiento de pila | `FreeRTOSConfig.h` | `configCHECK_FOR_STACK_OVERFLOW = 2`. |
| CPU % por tarea | `FreeRTOSConfig.h` | `configGENERATE_RUN_TIME_STATS = 1` con contador propio de 1 µs (`portALT_GET_RUN_TIME_COUNTER_VALUE`, ver `ulPortGetAltMicros()` en `main.c`). |

> Los periodos de las tareas, la ventana de limpieza del event group (3 s) y el periodo del timer (10 s) también se definen en `main.c`.

---

## Ejercicios propuestos

Ordenados de fácil a difícil:

1. **Cambiar umbrales.** Modifica `TEMP_ALARM_THRESHOLD` y `HUM_ALARM_THRESHOLD` y comprueba que las alarmas se disparan antes o después. ¿Qué pasa si subes el umbral por encima del valor máximo que puede generar el sensor aleatorio?

2. **Backpressure en la cola.** Cambia el `xQueueSend` de los sensores para que use `portMAX_DELAY` en lugar de un timeout corto, y ralentiza la tarea `monitor` (aumenta su retardo o hazle más trabajo). Observa cómo los productores se bloquean cuando la cola (8 elementos) se llena. *Ya implementado como demo: pulsa `k`.*

3. **Añadir un tercer sensor.** Copia el patrón de `temp`/`hum` para crear un sensor de presión con su propio periodo, añade el campo correspondiente al formato de la cola y amplía el `monitor` para imprimirlo y evaluar su umbral.

4. **Modo silencio.** Haz que la tarea `alarm`, mientras suena (durante esos 3 s), suspenda la tarea `stats` con `vTaskSuspend` y la reanude al limpiar el bit del event group. Comprueba que no se imprimen estadísticas con una alarma activa.

5. **Jugar con las prioridades.** Sube y baja las prioridades de `monitor`, `alarm` y `stats` y observa cómo cambia el orden de salida cuando varias tareas están listas al mismo tiempo. Explica el resultado en términos de planificación preemptiva de FreeRTOS. *Un ejemplo con `vTaskPrioritySet` ya está: pulsa `v`; la inversión de prioridades está en `i`.*

---

## Estructura del repositorio

```
freertos-sensor-station/
├── .github/workflows/ci.yml  # CI: build local + FetchContent + ASan, ctest y smoke test
├── CMakeLists.txt        # Build: FetchContent del kernel o ruta local + logic_tests
├── FreeRTOSConfig.h      # Configuración de FreeRTOS (heap, pila, tick, queue sets…)
├── LICENSE               # MIT
├── README.md             # Este archivo
├── .gitignore
├── logic.c / logic.h     # Funciones puras (sparklines, barras, umbrales) con tests
├── main.c                # Tareas, colas, buffers, watchdog, http, dashboard y controles
├── tests/test_logic.c    # Tests unitarios (ctest)
├── tools/plot_csv.py     # Gráfica SVG de readings.csv sin dependencias
└── docs/
    ├── demo.gif          # Captura del dashboard en marcha
    └── chart.svg         # Serie temporal generada por tools/plot_csv.py
```

---

## Licencia

Este proyecto se distribuye bajo la licencia **MIT**. Consulta el archivo [`LICENSE`](LICENSE).
