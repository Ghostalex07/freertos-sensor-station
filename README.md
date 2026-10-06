# Estación de sensores con FreeRTOS

Demo educativa de **FreeRTOS** que simula una estación de sensores de temperatura y humedad, corriendo en **Linux** sobre el puerto `GCC_POSIX` (las tareas FreeRTOS se ejecutan como hilos `pthread`, sin necesidad de placa ni emulador).

## Características destacadas

- 🐧 **Sin hardware**: usa el puerto `GCC_POSIX` de FreeRTOS, así que puedes probar multitarea real en tu propio PC.
- 📡 **Pipeline clásico de RTOS**: sensores → cola → monitor → semáforo → tarea de alarma → event group.
- ⏱️ **Periodicidad garantizada** con `vTaskDelayUntil` (no se acumula deriva).
- ⌨️ **Interfaz interactiva** por stdin en modo terminal raw: inyectar picos, suspender sensores, reiniciar contadores.
- 📊 **Telemetría propia**: heap libre, nº de tareas y mínimo de pila por tarea cada 5 s.
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

Pulsa las teclas mientras la aplicación está en ejecución:

| Tecla | Acción |
| :---: | --- |
| `t` | Inyecta una lectura de temperatura fuera de umbral (dispara alarma) |
| `h` | Inyecta una lectura de humedad fuera de umbral (dispara alarma) |
| `p` | Suspende las tareas de sensores (`vTaskSuspend`) |
| `c` | Reanuda las tareas de sensores (`vTaskResume`) |
| `r` | Reinicia el contador de alarmas |
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
  └────────────────┘        └──────────────────────────────┘
```

### Primitivas de FreeRTOS utilizadas

| Primitiva | APIs | Papel en la demo |
| --- | --- | --- |
| **Cola** | `xQueueCreate`, `xQueueSend`, `xQueueReceive` | Canaliza las lecturas de los sensores hacia el monitor (8 posiciones). |
| **Mutex** | `xSemaphoreCreateMutex`, `xSemaphoreTake`, `xSemaphoreGive` | Uno protege la salida compartida de `printf`, otro el contador de alarmas. |
| **Semáforo binario** | `xSemaphoreCreateBinary`, `xSemaphoreTake`, `xSemaphoreGive` | El monitor avisa a la tarea de alarma cuando detecta un umbral superado. |
| **Event group** | `xEventGroupCreate`, `xEventGroupSetBits`, `xEventGroupClearBits` | Marca que hay una alarma activa y la limpia pasados 3 s. |
| **Timer de software** | `xTimerCreate` | Cada 10 s inyecta un pico de temperatura que provoca alarma. |
| **Gestión de tareas** | `xTaskCreate`, `vTaskSuspend`, `vTaskResume` | Crea las 6 tareas; `p`/`c` congelan y reanudan los sensores. |
| **Periodicidad** | `vTaskDelayUntil` | Los sensores se ejecutan cada 700 ms / 1100 ms sin deriva. |
| **Diagnóstico** | `xPortGetFreeHeapSize`, `uxTaskGetStackHighWaterMark` | La tarea `stats` reporta heap libre y mínimo de pila usado. |

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

> Los periodos de las tareas, la ventana de limpieza del event group (3 s) y el periodo del timer (10 s) también se definen en `main.c`.

---

## Ejercicios propuestos

Ordenados de fácil a difícil:

1. **Cambiar umbrales.** Modifica `TEMP_ALARM_THRESHOLD` y `HUM_ALARM_THRESHOLD` y comprueba que las alarmas se disparan antes o después. ¿Qué pasa si subes el umbral por encima del valor máximo que puede generar el sensor aleatorio?

2. **Backpressure en la cola.** Cambia el `xQueueSend` de los sensores para que use `portMAX_DELAY` en lugar de un timeout corto, y ralentiza la tarea `monitor` (aumenta su retardo o hazle más trabajo). Observa cómo los productores se bloquean cuando la cola (8 elementos) se llena.

3. **Añadir un tercer sensor.** Copia el patrón de `temp`/`hum` para crear un sensor de presión con su propio periodo, añade el campo correspondiente al formato de la cola y amplía el `monitor` para imprimirlo y evaluar su umbral.

4. **Modo silencio.** Haz que la tarea `alarm`, mientras suena (durante esos 3 s), suspenda la tarea `stats` con `vTaskSuspend` y la reanude al limpiar el bit del event group. Comprueba que no se imprimen estadísticas con una alarma activa.

5. **Jugar con las prioridades.** Sube y baja las prioridades de `monitor`, `alarm` y `stats` y observa cómo cambia el orden de salida cuando varias tareas están listas al mismo tiempo. Explica el resultado en términos de planificación preemptiva de FreeRTOS.

---

## Estructura del repositorio

```
freertos-sensor-station/
├── CMakeLists.txt        # Build: FetchContent del kernel o ruta local
├── FreeRTOSConfig.h      # Configuración de FreeRTOS (heap, pila, tick…)
├── LICENSE               # MIT
├── README.md             # Este archivo
├── .gitignore
└── main.c                # Tareas, colas, semáforos, timers y controles
```

---

## Licencia

Este proyecto se distribuye bajo la licencia **MIT**. Consulta el archivo [`LICENSE`](LICENSE).
