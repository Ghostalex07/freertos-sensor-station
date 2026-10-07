#ifndef APP_SHARED_H
#define APP_SHARED_H

/* Global state and utilities shared across the demo modules.
 *
 * Everything used to live as `static` in main.c; when it was split into
 * several files this header became the "seam": objects with a single
 * owner stay `static` in their .c, and only what two or more modules
 * need is published here. The names never changed: the extraction was
 * mechanical. */

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

/* --- queues, semaphores and shared buffers --------------------------- */
extern QueueHandle_t xSensorQueue;
extern SemaphoreHandle_t xPrintMutex;
extern SemaphoreHandle_t xStateMutex;
extern SemaphoreHandle_t xDropSemaphore;
extern SemaphoreHandle_t xInvBinary;
extern SemaphoreHandle_t xInvMutex;
extern EventGroupHandle_t xInvEvents;
extern StreamBufferHandle_t xReadingStream;
extern MessageBufferHandle_t xEventMessage;

/* --- global state of the station ------------------------------------- */
extern SystemState_t xSystemState;

/* --- task handles touched by several modules ------------------------- */
extern TaskHandle_t xMonitorTaskHandle;
extern TaskHandle_t xStatsTaskHandle;

/* --- startup, terminal and demo flags -------------------------------- */
extern volatile sig_atomic_t xShutdownRequested;
extern volatile BaseType_t xDashboardEnabled;
extern volatile BaseType_t xMonitorHangDemo;
extern volatile BaseType_t xHttpEnabled;
extern volatile BaseType_t xSlowConsumer;
extern BaseType_t xStdinIsTty;
extern BaseType_t xStdoutIsTty;

/* --- shared utilities (defined in main.c) ---------------------------- */

/* Millisecond timestamp from the wall clock. */
long long llEpochMs( void );

/* Prints a line with a time prefix, under xPrintMutex. */
void vPrintLine( const char * pcLine );

/* Reports an event: stores it in xSystemState, forwards it to the CSV
 * logger and, when there is no dashboard, prints it on screen. */
#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
void vReportEvent( const char * pcFormat,
                   ... );

/* Clean process exit (q key or SIGINT/SIGTERM/SIGHUP). */
void vShutdown( void );

/* "HH:MM:SS" from the current tick. */
void vFormatUptime( char * pcBuffer,
                    size_t xBufferSize );

/* Sensor configuration (name, threshold, period...). */
const SensorConfig_t * pxGetSensorConfig( SensorId_t xId );

/* Queues a forced reading above the threshold on the main queue. */
BaseType_t xQueueForcedReading( const SensorConfig_t * pxConfig,
                                unsigned int * puiSeed );

/* Busy CPU (in tenths of %) from a task sample. */
unsigned long ulBusyFromSample( const TaskStatus_t * pxStatus,
                                UBaseType_t uxCount,
                                configRUN_TIME_COUNTER_TYPE ulTotal );

/* Busy CPU (in tenths of %) of the system right now. */
unsigned long ulBusyPercentX10( void );

#endif
