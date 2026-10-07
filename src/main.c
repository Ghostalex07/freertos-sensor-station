#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>
#include <event_groups.h>
#include <stream_buffer.h>
#include <message_buffer.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logic.h"
#include "app_shared.h"
#include "watchdog.h"
#include "logger_csv.h"
#include "http_server.h"
#include "dashboard.h"
#include "demos.h"

/* Estado que comparte main.c con el resto de modulos: todo lo declarado
 * en app_shared.h se define aqui SIN `static` (la costura entre ficheros).
 * Lo que solo usa main.c se queda `static` justo debajo. */

QueueHandle_t xSensorQueue;
SemaphoreHandle_t xPrintMutex;
SemaphoreHandle_t xStateMutex;
SemaphoreHandle_t xDropSemaphore;
SemaphoreHandle_t xInvBinary;
SemaphoreHandle_t xInvMutex;
EventGroupHandle_t xInvEvents;
StreamBufferHandle_t xReadingStream;
MessageBufferHandle_t xEventMessage;

SystemState_t xSystemState;

TaskHandle_t xMonitorTaskHandle;
TaskHandle_t xStatsTaskHandle;

volatile sig_atomic_t xShutdownRequested = 0;
volatile BaseType_t xDashboardEnabled = pdFALSE;
volatile BaseType_t xMonitorHangDemo = pdFALSE;
volatile BaseType_t xHttpEnabled = pdFALSE;
volatile BaseType_t xSlowConsumer = pdFALSE;
BaseType_t xStdinIsTty = pdFALSE;
BaseType_t xStdoutIsTty = pdFALSE;

/* --- estado de uso exclusivo de main.c -------------------------------- */

static QueueHandle_t xTempLatestQueue;
static QueueHandle_t xHumLatestQueue;
static QueueHandle_t xAggSet;
static SemaphoreHandle_t xAlarmSemaphore;
static EventGroupHandle_t xEventGroup;
static EventGroupHandle_t xSyncGroup;

static TaskHandle_t xTempTaskHandle;
static TaskHandle_t xHumTaskHandle;
static TaskHandle_t xAlarmTaskHandle;
static TaskHandle_t xCommandTaskHandle;
static TaskHandle_t xLoggerTaskHandle;
static TaskHandle_t xWatchdogTaskHandle;
static TaskHandle_t xHttpTaskHandle;
static TaskHandle_t xAggregatorTaskHandle;
static TaskHandle_t xSyncTaskHandle;
static TaskHandle_t xInvLowTaskHandle;
static TaskHandle_t xInvMedTaskHandle;
static TaskHandle_t xInvHighTaskHandle;

static unsigned int uiBaseSeed = 0;

long long llEpochMs( void )
{
    struct timespec xNow;

    ( void ) clock_gettime( CLOCK_REALTIME, &xNow );

    return ( ( long long ) xNow.tv_sec * 1000LL ) +
           ( ( long long ) xNow.tv_nsec / 1000000LL );
}

static struct termios xSavedTermios;
static BaseType_t xTerminalSaved = pdFALSE;

static unsigned int uiSpikeSeed = 1;

static const SensorConfig_t xTempSensor =
{
    .xId = SENSOR_TEMPERATURE,
    .pcName = "temperatura",
    .pcUnit = "C",
    .iAlarmThreshold = TEMP_ALARM_THRESHOLD,
    .xPeriodTicks = pdMS_TO_TICKS( TEMP_PERIOD_MS ),
    .iMin = 15,
    .iMax = 38
};

static const SensorConfig_t xHumSensor =
{
    .xId = SENSOR_HUMIDITY,
    .pcName = "humedad",
    .pcUnit = "%",
    .iAlarmThreshold = HUM_ALARM_THRESHOLD,
    .xPeriodTicks = pdMS_TO_TICKS( HUM_PERIOD_MS ),
    .iMin = 30,
    .iMax = 82
};

uint64_t ulPortGetAltMicros( void )
{
    static int64_t llBase = -1;
    struct timespec xNow;
    int64_t llMicros;

    ( void ) clock_gettime( CLOCK_MONOTONIC, &xNow );

    llMicros = ( ( int64_t ) xNow.tv_sec * 1000000LL ) +
               ( ( int64_t ) xNow.tv_nsec / 1000LL );

    if( llBase < 0 )
    {
        llBase = llMicros;
    }

    return ( uint64_t ) ( llMicros - llBase );
}

static void vRestoreTerminal( void )
{
    if( xTerminalSaved != pdFALSE )
    {
        ( void ) tcsetattr( STDIN_FILENO, TCSANOW, &xSavedTermios );
        xTerminalSaved = pdFALSE;
    }
}

static void vEnterRawTerminal( void )
{
    struct termios xRaw;

    if( isatty( STDIN_FILENO ) == 0 )
    {
        return;
    }

    if( tcgetattr( STDIN_FILENO, &xSavedTermios ) != 0 )
    {
        return;
    }

    xRaw = xSavedTermios;
    xRaw.c_lflag &= ( tcflag_t ) ~( ICANON | ECHO );
    xRaw.c_cc[ VMIN ] = 0;
    xRaw.c_cc[ VTIME ] = 0;

    if( tcsetattr( STDIN_FILENO, TCSANOW, &xRaw ) == 0 )
    {
        xTerminalSaved = pdTRUE;
        ( void ) atexit( vRestoreTerminal );
    }
}

static void vSignalHandler( int iSignalNumber )
{
    ( void ) iSignalNumber;
    xShutdownRequested = 1;
}

static void vInstallSignalHandlers( void )
{
    struct sigaction xAction;

    memset( &xAction, 0, sizeof( xAction ) );
    xAction.sa_handler = vSignalHandler;
    ( void ) sigemptyset( &xAction.sa_mask );
    xAction.sa_flags = 0;

    ( void ) sigaction( SIGINT, &xAction, NULL );
    ( void ) sigaction( SIGTERM, &xAction, NULL );
    ( void ) sigaction( SIGHUP, &xAction, NULL );
}

static void vFormatVa( char * pcBuffer,
                       size_t xBufferSize,
                       const char * pcFormat,
                       va_list xArgs )
{
    int iLength;

    iLength = vsnprintf( pcBuffer, xBufferSize, pcFormat, xArgs );

    if( iLength < 0 )
    {
        pcBuffer[ 0 ] = '\0';
    }
    else if( ( ( size_t ) iLength >= xBufferSize ) && ( xBufferSize >= 4 ) )
    {
        memcpy( pcBuffer + xBufferSize - 4, "...", 4 );
    }
}

void vPrintLine( const char * pcLine )
{
    xSemaphoreTake( xPrintMutex, portMAX_DELAY );
    printf( "[%4lu s] %s\n",
            ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ ),
            pcLine );
    fflush( stdout );
    xSemaphoreGive( xPrintMutex );
}

#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
static void vPrintFormat( const char * pcFormat,
                          ... )
{
    char pcBuffer[ PRINT_BUFFER_SIZE ];
    va_list xArgs;

    va_start( xArgs, pcFormat );
    vFormatVa( pcBuffer, sizeof( pcBuffer ), pcFormat, xArgs );
    va_end( xArgs );

    vPrintLine( pcBuffer );
}

#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
void vReportEvent( const char * pcFormat,
                   ... )
{
    char pcBuffer[ PRINT_BUFFER_SIZE ];
    va_list xArgs;

    va_start( xArgs, pcFormat );
    vFormatVa( pcBuffer, sizeof( pcBuffer ), pcFormat, xArgs );
    va_end( xArgs );

    xSemaphoreTake( xStateMutex, portMAX_DELAY );
    ( void ) strncpy( xSystemState.pcLastEvent, pcBuffer, sizeof( xSystemState.pcLastEvent ) - 1 );
    xSystemState.pcLastEvent[ sizeof( xSystemState.pcLastEvent ) - 1 ] = '\0';
    xSemaphoreGive( xStateMutex );

    if( xEventMessage != NULL )
    {
        /* vFormatVa ya trunca: strlen <= PRINT_BUFFER_SIZE - 1 siempre. */
        ( void ) xMessageBufferSend( xEventMessage, pcBuffer, strlen( pcBuffer ), 0 );
    }

    if( xDashboardEnabled == pdFALSE )
    {
        vPrintLine( pcBuffer );
    }
}

void vShutdown( void )
{
    /* Sin xPrintMutex: al salir no se debe bloquear (si stdout se queda
     * sin espacio el Take con portMAX_DELAY colgaria el apagado). */
    if( xDashboardEnabled != pdFALSE )
    {
        printf( "\x1b[2J\x1b[H" );
    }

    printf( "[%4lu s] saliendo...\n",
            ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ ) );
    fflush( stdout );

    vRestoreTerminal();
    _exit( 0 );
}

void vFormatUptime( char * pcBuffer,
                    size_t xBufferSize )
{
    unsigned long ulSeconds = ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ );

    ( void ) snprintf( pcBuffer, xBufferSize, "%02lu:%02lu:%02lu",
                       ulSeconds / 3600UL,
                       ( ulSeconds / 60UL ) % 60UL,
                       ulSeconds % 60UL );
}

const SensorConfig_t * pxGetSensorConfig( SensorId_t xId )
{
    return ( xId == SENSOR_TEMPERATURE ) ? &xTempSensor : &xHumSensor;
}

BaseType_t xQueueForcedReading( const SensorConfig_t * pxConfig,
                                unsigned int * puiSeed )
{
    SensorReading_t xReading;

    xReading.xId = pxConfig->xId;
    xReading.iValue = pxConfig->iAlarmThreshold + FORCED_OVERSHOOT + ( int ) ( rand_r( puiSeed ) % FORCED_JITTER );
    xReading.ulSequence = 0;

    return xQueueSend( xSensorQueue, &xReading, 0 );
}

static void vSensorTask( void * pvParameters )
{
    const SensorConfig_t * pxConfig = ( const SensorConfig_t * ) pvParameters;
    SensorReading_t xReading;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    unsigned int uiSeed;

    if( uiBaseSeed != 0U )
    {
        uiSeed = uiBaseSeed ^ ( unsigned int ) pxConfig->xId;
    }
    else
    {
        uiSeed = ( unsigned int ) ( ( unsigned long ) pvParameters ^ ( unsigned long ) xLastWakeTime );
    }

    for( ; ; )
    {
        vWatchdogBeat( ( pxConfig->xId == SENSOR_TEMPERATURE ) ? WD_TEMP : WD_HUM );

        xSemaphoreTake( xStateMutex, portMAX_DELAY );

        if( xSystemState.xSensorsPaused != pdFALSE )
        {
            xSemaphoreGive( xStateMutex );
            vTaskDelay( pdMS_TO_TICKS( PAUSE_POLL_MS ) );
            xLastWakeTime = xTaskGetTickCount();
            continue;
        }

        xReading.xId = pxConfig->xId;
        xReading.iValue = iRandomRange( pxConfig->iMin, pxConfig->iMax, &uiSeed );
        xReading.ulSequence = ++xSystemState.ulReadings;
        xSemaphoreGive( xStateMutex );

        if( xQueueSend( xSensorQueue, &xReading, 0 ) != pdPASS )
        {
            xSemaphoreGive( xDropSemaphore );
            vReportEvent( "cola de sensores llena: lectura descartada" );
        }

        /* Cola de longitud 1: xQueueOverwrite mantiene el ultimo valor
         * accesible sin consumirlo (se publica aunque la cola principal
         * estuviera llena). */
        {
            QueueHandle_t xLatest = ( pxConfig->xId == SENSOR_TEMPERATURE ) ?
                                    xTempLatestQueue : xHumLatestQueue;

            ( void ) xQueueOverwrite( xLatest, &xReading );
        }

        /* Cada sensor marca su bit; la tarea sync espera los dos (event group). */
        ( void ) xEventGroupSetBits( xSyncGroup,
                                     ( pxConfig->xId == SENSOR_TEMPERATURE ) ?
                                     SYNC_TEMP_BIT : SYNC_HUM_BIT );

        if( xTaskDelayUntil( &xLastWakeTime, pxConfig->xPeriodTicks ) == pdFALSE )
        {
            xLastWakeTime = xTaskGetTickCount();
        }
    }
}

static void vMonitorTask( void * pvParameters )
{
    SensorReading_t xReading;
    ReadingRecord_t xRecord;
    const SensorConfig_t * pxConfig;
    BaseType_t xIsAlarm;

    ( void ) pvParameters;

    for( ; ; )
    {
        if( xMonitorHangDemo != pdFALSE )
        {
            /* Demo 'w': simula la tarea colgada sin suspenderla (nunca
             * retiene mutex ni cola): no late el watchdog ni consume. */
            vTaskDelay( pdMS_TO_TICKS( PAUSE_POLL_MS ) );
            continue;
        }

        vWatchdogBeat( WD_MONITOR );

        if( xQueueReceive( xSensorQueue, &xReading, portMAX_DELAY ) == pdPASS )
        {
            pxConfig = pxGetSensorConfig( xReading.xId );
            xIsAlarm = ( iIsAlarmValue( xReading.iValue, pxConfig->iAlarmThreshold ) != 0 ) ? pdTRUE : pdFALSE;

            xSemaphoreTake( xStateMutex, portMAX_DELAY );

            if( xReading.xId == SENSOR_TEMPERATURE )
            {
                xSystemState.iLastTempValue = xReading.iValue;
                xSystemState.aiTempHistory[ xSystemState.uiTempHistNext % SPARK_HISTORY ] = xReading.iValue;
                xSystemState.uiTempHistNext++;
            }
            else
            {
                xSystemState.iLastHumValue = xReading.iValue;
                xSystemState.aiHumHistory[ xSystemState.uiHumHistNext % SPARK_HISTORY ] = xReading.iValue;
                xSystemState.uiHumHistNext++;
            }

            xSemaphoreGive( xStateMutex );

            xRecord.llEpochMs = llEpochMs();
            xRecord.xId = xReading.xId;
            xRecord.iValue = xReading.iValue;
            xRecord.ulSequence = xReading.ulSequence;
            xRecord.xIsAlarm = xIsAlarm;

            if( xStreamBufferSend( xReadingStream, &xRecord, sizeof( xRecord ), 0 ) != sizeof( xRecord ) )
            {
                xSemaphoreTake( xStateMutex, portMAX_DELAY );
                xSystemState.ulLogDrops++;
                xSemaphoreGive( xStateMutex );
            }

            if( xDashboardEnabled == pdFALSE )
            {
                vPrintFormat( "%-10s #%lu = %d %s%s",
                              pxConfig->pcName,
                              xReading.ulSequence,
                              xReading.iValue,
                              pxConfig->pcUnit,
                              ( xIsAlarm == pdTRUE ) ? "  <-- ALARMA" : "" );
            }

            if( xIsAlarm == pdTRUE )
            {
                xSemaphoreGive( xAlarmSemaphore );
            }

            if( xSlowConsumer != pdFALSE )
            {
                /* Demo de presion inversa: consumidor lento, la cola se llena
                 * y los descartados suben mientras dure la tecla k. */
                vTaskDelay( pdMS_TO_TICKS( SLOW_CONSUMER_MS ) );
            }
        }
    }
}

static void vAlarmTask( void * pvParameters )
{
    unsigned long ulAlarmTotal;

    ( void ) pvParameters;

    for( ; ; )
    {
        if( xSemaphoreTake( xAlarmSemaphore, pdMS_TO_TICKS( 1000 ) ) == pdPASS )
        {
            xSemaphoreTake( xStateMutex, portMAX_DELAY );
            xSystemState.ulAlarms++;
            ulAlarmTotal = xSystemState.ulAlarms;
            xSystemState.xAlarmActive = pdTRUE;
            xSemaphoreGive( xStateMutex );

            xEventGroupSetBits( xEventGroup, ALARM_EVENT_BIT );
            xTaskNotifyGive( xStatsTaskHandle );

            /* Por vReportEvent: actualiza ultimo evento, mensaje y events.csv. */
            vReportEvent( "!! ALARMA activa: umbral superado (alarmas totales: %lu) !!",
                          ulAlarmTotal );

            vTaskDelay( pdMS_TO_TICKS( ALARM_HOLD_MS ) );

            xSemaphoreTake( xStateMutex, portMAX_DELAY );
            xSystemState.xAlarmActive = pdFALSE;
            xSemaphoreGive( xStateMutex );

            xEventGroupClearBits( xEventGroup, ALARM_EVENT_BIT );
            xTaskNotifyGive( xStatsTaskHandle );
        }

        vWatchdogBeat( WD_ALARM );
    }
}

static void vAggregatorTask( void * pvParameters )
{
    QueueHandle_t xSignaled;
    SensorReading_t xReading;
    SensorReading_t xLastTemp;
    SensorReading_t xLastHum;
    BaseType_t xHaveTemp = pdFALSE;
    BaseType_t xHaveHum = pdFALSE;

    ( void ) pvParameters;

    for( ; ; )
    {
        /* Queue set: espera a que actualice cualquiera de las dos colas. */
        xSignaled = ( QueueHandle_t ) xQueueSelectFromSet( xAggSet, pdMS_TO_TICKS( 1000 ) );

        if( xSignaled == NULL )
        {
            continue;
        }

        /* xQueuePeek mira el ultimo valor SIN consumirlo; xQueueReceive lo
         * retira para que el set pueda senalar de nuevo. */
        if( ( xQueuePeek( xSignaled, &xReading, 0 ) == pdPASS ) &&
            ( xQueueReceive( xSignaled, &xReading, 0 ) == pdPASS ) )
        {
            if( xReading.xId == SENSOR_TEMPERATURE )
            {
                xLastTemp = xReading;
                xHaveTemp = pdTRUE;
            }
            else
            {
                xLastHum = xReading;
                xHaveHum = pdTRUE;
            }

            if( ( xHaveTemp != pdFALSE ) && ( xHaveHum != pdFALSE ) &&
                ( xDashboardEnabled == pdFALSE ) )
            {
                vPrintFormat( "aggreg: par listo temp=%d hum=%d (queue set + peek + receive)",
                              xLastTemp.iValue, xLastHum.iValue );
                xHaveTemp = pdFALSE;
                xHaveHum = pdFALSE;
            }
        }
    }
}

static void vSyncTask( void * pvParameters )
{
    EventBits_t xBits;

    ( void ) pvParameters;

    for( ; ; )
    {
        /* Espera a que AMBOS sensores hayan publicado; clear-on-exit
         * borra los dos bits para el ciclo siguiente. */
        xBits = xEventGroupWaitBits( xSyncGroup,
                                     SYNC_TEMP_BIT | SYNC_HUM_BIT,
                                     pdTRUE,
                                     pdTRUE,
                                     portMAX_DELAY );

        if( ( ( xBits & ( SYNC_TEMP_BIT | SYNC_HUM_BIT ) ) ==
             ( SYNC_TEMP_BIT | SYNC_HUM_BIT ) ) &&
            ( xDashboardEnabled == pdFALSE ) )
        {
            vPrintFormat( "sync: lecturas de temp y hum coordinadas (event group, 2 bits)" );
        }
    }
}

unsigned long ulBusyFromSample( const TaskStatus_t * pxStatus,
                                UBaseType_t uxCount,
                                configRUN_TIME_COUNTER_TYPE ulTotal )
{
    unsigned long ulIdleTenths = 0;
    unsigned long ulSumTenths = 0;
    BaseType_t xFoundIdle = pdFALSE;
    UBaseType_t uxIndex;

    if( ( uxCount == 0 ) || ( ulTotal == 0 ) )
    {
        return 0UL;
    }

    for( uxIndex = 0; uxIndex < uxCount; uxIndex++ )
    {
        unsigned long ulTenths = ( ( unsigned long ) pxStatus[ uxIndex ].ulRunTimeCounter * 1000UL ) / ( unsigned long ) ulTotal;

        if( strcmp( pxStatus[ uxIndex ].pcTaskName, "IDLE" ) == 0 )
        {
            ulIdleTenths = ulTenths;
            xFoundIdle = pdTRUE;
        }
        else
        {
            ulSumTenths += ulTenths;
        }
    }

    if( xFoundIdle != pdFALSE )
    {
        return ( ulIdleTenths < 1000UL ) ? ( 1000UL - ulIdleTenths ) : 0UL;
    }

    if( ulSumTenths > 1000UL )
    {
        ulSumTenths = 1000UL;
    }

    return ulSumTenths;
}

unsigned long ulBusyPercentX10( void )
{
    TaskStatus_t xStatus[ TASK_STATUS_MAX ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;

    uxCount = uxTaskGetSystemState( xStatus, TASK_STATUS_MAX, &ulTotal );

    return ulBusyFromSample( xStatus, uxCount, ulTotal );
}

static void vRefreshCpuState( void )
{
    TaskStatus_t xStatus[ TASK_STATUS_MAX ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;
    UBaseType_t uxIndex;
    unsigned long ulTemp = 0UL;
    unsigned long ulHum = 0UL;
    unsigned long ulMonitor = 0UL;
    unsigned long ulAlarm = 0UL;
    unsigned long ulTenths;

    uxCount = uxTaskGetSystemState( xStatus, TASK_STATUS_MAX, &ulTotal );

    if( ( uxCount > 0U ) && ( ulTotal != 0U ) )
    {
        for( uxIndex = 0; uxIndex < uxCount; uxIndex++ )
        {
            ulTenths = ( ( unsigned long ) xStatus[ uxIndex ].ulRunTimeCounter * 1000UL ) /
                       ( unsigned long ) ulTotal;

            if( strcmp( xStatus[ uxIndex ].pcTaskName, "temp" ) == 0 )
            {
                ulTemp = ulTenths;
            }
            else if( strcmp( xStatus[ uxIndex ].pcTaskName, "hum" ) == 0 )
            {
                ulHum = ulTenths;
            }
            else if( strcmp( xStatus[ uxIndex ].pcTaskName, "monitor" ) == 0 )
            {
                ulMonitor = ulTenths;
            }
            else if( strcmp( xStatus[ uxIndex ].pcTaskName, "alarm" ) == 0 )
            {
                ulAlarm = ulTenths;
            }
        }
    }

    xSemaphoreTake( xStateMutex, portMAX_DELAY );
    xSystemState.ulCpuTempX10 = ulTemp;
    xSystemState.ulCpuHumX10 = ulHum;
    xSystemState.ulCpuMonitorX10 = ulMonitor;
    xSystemState.ulCpuAlarmX10 = ulAlarm;
    xSemaphoreGive( xStateMutex );
}

static void vStatsTask( void * pvParameters )
{
    unsigned long ulReadings;
    unsigned long ulAlarms;
    unsigned long ulDropped;
    unsigned long ulSpikes;
    unsigned long ulBusy;
    unsigned long ulLogDrops;
    unsigned long ulWdFails;
    BaseType_t xWdActive;
    EventBits_t xBits;

    ( void ) pvParameters;

    for( ; ; )
    {
        vWatchdogBeat( WD_STATS );
        vRefreshCpuState();

        if( xDashboardEnabled != pdFALSE )
        {
            ( void ) xTaskNotifyWait( 0, 0xffffffffUL, NULL, pdMS_TO_TICKS( DASHBOARD_PERIOD_MS ) );
        }
        else
        {
            vTaskDelay( pdMS_TO_TICKS( STATS_PERIOD_MS ) );
        }

        while( xSemaphoreTake( xDropSemaphore, 0 ) == pdPASS )
        {
            xSemaphoreTake( xStateMutex, portMAX_DELAY );
            xSystemState.ulDropped++;
            xSemaphoreGive( xStateMutex );
        }

        if( xDashboardEnabled != pdFALSE )
        {
            vDashboardDraw();
        }
        else
        {
            xSemaphoreTake( xStateMutex, portMAX_DELAY );
            ulReadings = xSystemState.ulReadings;
            ulAlarms = xSystemState.ulAlarms;
            ulDropped = xSystemState.ulDropped;
            ulSpikes = xSystemState.ulSpikes;
            ulLogDrops = xSystemState.ulLogDrops;
            ulWdFails = xSystemState.ulWatchdogFails;
            xWdActive = xSystemState.xWatchdogActive;
            xSemaphoreGive( xStateMutex );

            xBits = xEventGroupGetBits( xEventGroup );

            vPrintFormat( "stats: lecturas=%lu alarmas=%lu perdidas=%lu picos=%lu cola=%u/%u heap_libre=%zu B tareas=%u eventos=%s",
                          ulReadings,
                          ulAlarms,
                          ulDropped,
                          ulSpikes,
                          ( unsigned int ) uxQueueMessagesWaiting( xSensorQueue ),
                          ( unsigned int ) SENSOR_QUEUE_LENGTH,
                          xPortGetFreeHeapSize(),
                          ( unsigned int ) uxTaskGetNumberOfTasks(),
                          ( xBits & ALARM_EVENT_BIT ) ? "ALARMA" : "normal" );

            ulBusy = ulBusyPercentX10();
            vPrintFormat( "cpu: sistema ocupado %lu.%lu%%", ulBusy / 10UL, ulBusy % 10UL );

            vPrintFormat( "watchdog: %s  fallos=%lu  log_perdidas=%lu",
                          ( xWdActive != pdFALSE ) ? "ALERTA" : "ok",
                          ulWdFails,
                          ulLogDrops );

            vPrintFormat( "stack minima libre (palabras): monitor=%u alarm=%u stats=%u",
                          ( unsigned int ) uxTaskGetStackHighWaterMark( xMonitorTaskHandle ),
                          ( unsigned int ) uxTaskGetStackHighWaterMark( xAlarmTaskHandle ),
                          ( unsigned int ) uxTaskGetStackHighWaterMark( xStatsTaskHandle ) );
        }
    }
}

static void vSpikeTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;

    if( xQueueForcedReading( &xTempSensor, &uiSpikeSeed ) == pdPASS )
    {
        /* Sin mutex: el callback corre en Tmr Svc (prioridad maxima) y
         * bloquearse aqui pararia todos los timers. Escritura atomica;
         * las leidas (stats, HTTP) ocurren bajo xStateMutex. */
        ( void ) __atomic_add_fetch( &xSystemState.ulSpikes, 1U, __ATOMIC_RELAXED );
    }
    else
    {
        ( void ) xSemaphoreGive( xDropSemaphore );
    }
}

void vApplicationStackOverflowHook( TaskHandle_t xTask,
                                    char * pcTaskName )
{
    ( void ) xTask;
    fprintf( stderr, "STACK OVERFLOW en tarea %s\n", pcTaskName );
    fflush( stderr );
    vRestoreTerminal();
    abort();
}

void vApplicationMallocFailedHook( void )
{
    fprintf( stderr, "pvPortMalloc ha fallado\n" );
    fflush( stderr );
    vRestoreTerminal();
    abort();
}

void vAssertCalled( const char * pcFile,
                    int iLine )
{
    fprintf( stderr, "configASSERT fallido en %s:%d\n", pcFile, iLine );
    fflush( stderr );
    vRestoreTerminal();
    abort();
}

int main( void )
{
    TimerHandle_t xSpikeTimer;
    BaseType_t xResult;

    vInstallSignalHandlers();

    xStdinIsTty = ( isatty( STDIN_FILENO ) != 0 ) ? pdTRUE : pdFALSE;
    xStdoutIsTty = ( isatty( STDOUT_FILENO ) != 0 ) ? pdTRUE : pdFALSE;
    xDashboardEnabled = ( ( xStdinIsTty != pdFALSE ) && ( xStdoutIsTty != pdFALSE ) ) ? pdTRUE : pdFALSE;

    vEnterRawTerminal();

    {
        const char * pcSeedEnv = getenv( "SENSOR_STATION_SEED" );

        if( pcSeedEnv != NULL )
        {
            uiBaseSeed = ( unsigned int ) strtoul( pcSeedEnv, NULL, 0 );
            uiSpikeSeed = uiBaseSeed ^ 0x5a5a5a5aU;
        }
        else
        {
            uiSpikeSeed = ( unsigned int ) time( NULL ) ^ 0x5a5a5a5aU;
        }
    }

    if( xDashboardEnabled == pdFALSE )
    {
        printf( "=== Estacion de sensores FreeRTOS (puerto POSIX) ===\n" );
        printf( "%s\n\n", pcHelpText );
    }

    xSensorQueue = xQueueCreate( SENSOR_QUEUE_LENGTH, sizeof( SensorReading_t ) );
    xTempLatestQueue = xQueueCreate( 1, sizeof( SensorReading_t ) );
    xHumLatestQueue = xQueueCreate( 1, sizeof( SensorReading_t ) );
    xPrintMutex = xSemaphoreCreateMutex();
    xStateMutex = xSemaphoreCreateMutex();
    xAlarmSemaphore = xSemaphoreCreateBinary();
    xDropSemaphore = xSemaphoreCreateCounting( DROP_COUNT_MAX, 0 );
    xInvBinary = xSemaphoreCreateBinary();
    xInvMutex = xSemaphoreCreateMutex();
    /* El semaforo binario arranca vacio: lo dejamos disponible para que
     * la tarea BAJA pueda cogerlo en la demo de inversion de prioridades. */
    configASSERT( xSemaphoreGive( xInvBinary ) == pdPASS );
    xEventGroup = xEventGroupCreate();
    xSyncGroup = xEventGroupCreate();
    xInvEvents = xEventGroupCreate();
    xReadingStream = xStreamBufferCreate( STREAM_LENGTH, 1 );
    xEventMessage = xMessageBufferCreate( MESSAGE_BUFFER_LENGTH );

    configASSERT( ( xSensorQueue != NULL ) && ( xTempLatestQueue != NULL ) &&
                  ( xHumLatestQueue != NULL ) && ( xPrintMutex != NULL ) &&
                  ( xStateMutex != NULL ) && ( xAlarmSemaphore != NULL ) &&
                  ( xDropSemaphore != NULL ) && ( xInvBinary != NULL ) &&
                  ( xInvMutex != NULL ) && ( xEventGroup != NULL ) &&
                  ( xSyncGroup != NULL ) && ( xInvEvents != NULL ) &&
                  ( xReadingStream != NULL ) && ( xEventMessage != NULL ) );

    xAggSet = xQueueCreateSet( 2 );
    configASSERT( xAggSet != NULL );
    configASSERT( xQueueAddToSet( xTempLatestQueue, xAggSet ) == pdPASS );
    configASSERT( xQueueAddToSet( xHumLatestQueue, xAggSet ) == pdPASS );

    memset( &xSystemState, 0, sizeof( xSystemState ) );
    strcpy( xSystemState.pcLastEvent, "sistema iniciado" );

    xResult = xTaskCreate( vSensorTask, "temp", configMINIMAL_STACK_SIZE,
                            ( void * ) &xTempSensor, PRIORITY_SENSOR, &xTempTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vSensorTask, "hum", configMINIMAL_STACK_SIZE,
                            ( void * ) &xHumSensor, PRIORITY_SENSOR, &xHumTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vMonitorTask, "monitor", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_MONITOR, &xMonitorTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vAlarmTask, "alarm", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_ALARM, &xAlarmTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vStatsTask, "stats", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_STATS, &xStatsTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vCommandTask, "command", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_COMMAND, &xCommandTaskHandle );
    configASSERT( xResult == pdPASS );

    /* Logger con alojamiento estatico (TCB y pila propios, sin pvPortMalloc). */
    xLoggerTaskHandle = xTaskCreateStatic( vLoggerTask, "logger", configMINIMAL_STACK_SIZE,
                                            NULL, PRIORITY_LOGGER,
                                            xLoggerStack, &xLoggerTcb );
    configASSERT( xLoggerTaskHandle != NULL );

    xResult = xTaskCreate( vWatchdogTask, "watchdog", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_WATCHDOG, &xWatchdogTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vHttpTask, "http", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_HTTP, &xHttpTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vAggregatorTask, "aggreg", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_STATS, &xAggregatorTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vSyncTask, "sync", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_STATS, &xSyncTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vInvLowTask, "inv-low", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_STATS, &xInvLowTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vInvMedTask, "inv-med", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_SENSOR, &xInvMedTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vInvHighTask, "inv-high", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_MONITOR, &xInvHighTaskHandle );
    configASSERT( xResult == pdPASS );

    xSpikeTimer = xTimerCreate( "spike", pdMS_TO_TICKS( SPIKE_PERIOD_MS ),
                                pdTRUE, NULL, vSpikeTimerCallback );
    configASSERT( xSpikeTimer != NULL );

    xResult = xTimerStart( xSpikeTimer, 0 );
    configASSERT( xResult == pdPASS );

    vTaskStartScheduler();

    fprintf( stderr, "Error: el scheduler no arranco\n" );
    return 1;
}
