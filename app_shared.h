#ifndef APP_SHARED_H
#define APP_SHARED_H

/* Estado global y utilidades compartidas entre los modulos de la demo.
 *
 * Antes todo vivia como `static` en main.c; al repartirlo en ficheros
 * se deja aqui la "costura": los objetos con un unico dueno siguen
 * `static` en su .c, y solo se publica lo que usan dos o mas modulos.
 * Los nombres no cambian: la extraccion fue mecanica. */

#include <signal.h>
#include <stddef.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <event_groups.h>
#include <stream_buffer.h>
#include <message_buffer.h>

#include "app_types.h"

/* --- colas, semaforos y buffers compartidos -------------------------- */
extern QueueHandle_t xSensorQueue;
extern SemaphoreHandle_t xPrintMutex;
extern SemaphoreHandle_t xStateMutex;
extern SemaphoreHandle_t xDropSemaphore;
extern SemaphoreHandle_t xInvBinary;
extern SemaphoreHandle_t xInvMutex;
extern EventGroupHandle_t xInvEvents;
extern StreamBufferHandle_t xReadingStream;
extern MessageBufferHandle_t xEventMessage;

/* --- estado global de la estacion ------------------------------------ */
extern SystemState_t xSystemState;

/* --- handles de tareas que tocan varios modulos ---------------------- */
extern TaskHandle_t xMonitorTaskHandle;
extern TaskHandle_t xStatsTaskHandle;

/* --- flags de arranque, terminal y demos ----------------------------- */
extern volatile sig_atomic_t xShutdownRequested;
extern volatile BaseType_t xDashboardEnabled;
extern volatile BaseType_t xMonitorHangDemo;
extern volatile BaseType_t xHttpEnabled;
extern volatile BaseType_t xSlowConsumer;
extern BaseType_t xStdinIsTty;
extern BaseType_t xStdoutIsTty;

/* --- utilidades compartidas (definidas en main.c) -------------------- */

/* Marca de tiempo en milisegundos desde el reloj de pared. */
long long llEpochMs( void );

/* Imprime una linea con prefijo de tiempo, bajo xPrintMutex. */
void vPrintLine( const char * pcLine );

/* Registra un evento: lo deja en xSystemState, lo manda al logger CSV
 * y, si no hay dashboard, lo imprime en pantalla. */
#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
void vReportEvent( const char * pcFormat,
                   ... );

/* Salida ordenada del proceso (senal q o SIGINT/SIGTERM/SIGHUP). */
void vShutdown( void );

/* "HH:MM:SS" a partir del tick actual. */
void vFormatUptime( char * pcBuffer,
                    size_t xBufferSize );

/* Configuracion (nombre, umbral, periodo...) de un sensor. */
const SensorConfig_t * pxGetSensorConfig( SensorId_t xId );

/* Envia a la cola principal una lectura forzada por encima del umbral. */
BaseType_t xQueueForcedReading( const SensorConfig_t * pxConfig,
                                unsigned int * puiSeed );

/* CPU ocupada (en decimas de %) a partir de una muestra de tareas. */
unsigned long ulBusyFromSample( const TaskStatus_t * pxStatus,
                                UBaseType_t uxCount,
                                configRUN_TIME_COUNTER_TYPE ulTotal );

/* CPU ocupada (en decimas de %) del sistema en este instante. */
unsigned long ulBusyPercentX10( void );

#endif
