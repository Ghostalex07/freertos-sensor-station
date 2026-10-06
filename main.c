#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>
#include <event_groups.h>
#include <stream_buffer.h>
#include <message_buffer.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logic.h"

#define SENSOR_QUEUE_LENGTH    8
#define TEMP_ALARM_THRESHOLD   40
#define HUM_ALARM_THRESHOLD    85
#define TEMP_PERIOD_MS         700
#define HUM_PERIOD_MS          1100
#define ALARM_HOLD_MS          3000
#define STATS_PERIOD_MS        5000
#define SPIKE_PERIOD_MS        10000
#define PAUSE_POLL_MS          100
#define COMMAND_POLL_MS        50
#define ALARM_EVENT_BIT        ( 1 << 0 )

#define DASHBOARD_PERIOD_MS    500
#define WATCHDOG_CHECK_MS      500
#define WATCHDOG_TIMEOUT_MS    5000
#define HTTP_PORT              8080
#define HTTP_POLL_MS           50
#define STREAM_LENGTH          ( 16 * 64 )
#define MESSAGE_BUFFER_LENGTH  1024
#define SPARK_HISTORY          32
#define CSV_READINGS_PATH      "readings.csv"
#define CSV_EVENTS_PATH        "events.csv"

#define FORCED_OVERSHOOT       5
#define FORCED_JITTER          10
#define PRINT_BUFFER_SIZE      160
#define EVENT_BUFFER_SIZE      96
#define CPU_BAR_WIDTH          24
#define VALUE_BAR_WIDTH        28

#define SLOW_CONSUMER_MS       2000
#define PRIORITY_DEMO_MS       5000
#define CSV_ROTATE_BYTES       ( 64 * 1024 )
#define INV_WORK_ITERS         5000
#define INV_MED_MS             1000

#define SYNC_TEMP_BIT          ( 1 << 0 )
#define SYNC_HUM_BIT           ( 1 << 1 )

#define INV_LOW_GO             ( 1 << 0 )
#define INV_MED_GO             ( 1 << 1 )
#define INV_HIGH_GO            ( 1 << 2 )
#define INV_HELD               ( 1 << 3 )
#define INV_MED_DONE           ( 1 << 4 )
#define INV_HIGH_DONE          ( 1 << 5 )
#define INV_ALL_CLEAR          ( INV_HELD | INV_MED_DONE | INV_HIGH_DONE )

#define PRIORITY_ALARM         4
#define PRIORITY_MONITOR       3
#define PRIORITY_SENSOR        2
#define PRIORITY_COMMAND       2
#define PRIORITY_LOGGER        2
#define PRIORITY_STATS         1
#define PRIORITY_WATCHDOG      1
#define PRIORITY_HTTP          1

#define COLOR_RESET            "\x1b[0m"
#define COLOR_BOLD             "\x1b[1m"
#define COLOR_RED              "\x1b[31m"
#define COLOR_GREEN            "\x1b[32m"
#define COLOR_YELLOW           "\x1b[33m"

typedef enum
{
    SENSOR_TEMPERATURE = 0,
    SENSOR_HUMIDITY
} SensorId_t;

typedef struct
{
    SensorId_t xId;
    int iValue;
    unsigned long ulSequence;
} SensorReading_t;

typedef struct
{
    long long llEpochMs;
    SensorId_t xId;
    int iValue;
    unsigned long ulSequence;
    BaseType_t xIsAlarm;
} ReadingRecord_t;

typedef enum
{
    WD_TEMP = 0,
    WD_HUM,
    WD_MONITOR,
    WD_ALARM,
    WD_STATS,
    WD_LOGGER,
    WD_HTTP,
    WD_COUNT
} WatchdogId_t;

typedef struct
{
    SensorId_t xId;
    const char * pcName;
    const char * pcUnit;
    int iAlarmThreshold;
    TickType_t xPeriodTicks;
    int iMin;
    int iMax;
} SensorConfig_t;

typedef struct
{
    int iLastTempValue;
    int iLastHumValue;
    unsigned long ulReadings;
    unsigned long ulAlarms;
    unsigned long ulDropped;
    unsigned long ulSpikes;
    unsigned long ulLogDrops;
    unsigned long ulWatchdogFails;
    BaseType_t xAlarmActive;
    BaseType_t xSensorsPaused;
    BaseType_t xWatchdogActive;
    int aiTempHistory[ SPARK_HISTORY ];
    int aiHumHistory[ SPARK_HISTORY ];
    unsigned int uiTempHistNext;
    unsigned int uiHumHistNext;
    unsigned long ulCpuTempX10;
    unsigned long ulCpuHumX10;
    unsigned long ulCpuMonitorX10;
    unsigned long ulCpuAlarmX10;
    char pcLastEvent[ EVENT_BUFFER_SIZE ];
} SystemState_t;

static QueueHandle_t xSensorQueue;
static QueueHandle_t xTempLatestQueue;
static QueueHandle_t xHumLatestQueue;
static QueueHandle_t xAggSet;
static SemaphoreHandle_t xPrintMutex;
static SemaphoreHandle_t xStateMutex;
static SemaphoreHandle_t xAlarmSemaphore;
static SemaphoreHandle_t xDropSemaphore;
static SemaphoreHandle_t xInvBinary;
static SemaphoreHandle_t xInvMutex;
static EventGroupHandle_t xEventGroup;
static EventGroupHandle_t xSyncGroup;
static EventGroupHandle_t xInvEvents;
static StreamBufferHandle_t xReadingStream;
static MessageBufferHandle_t xEventMessage;

static SystemState_t xSystemState;

static TaskHandle_t xTempTaskHandle;
static TaskHandle_t xHumTaskHandle;
static TaskHandle_t xMonitorTaskHandle;
static TaskHandle_t xAlarmTaskHandle;
static TaskHandle_t xStatsTaskHandle;
static TaskHandle_t xCommandTaskHandle;
static TaskHandle_t xLoggerTaskHandle;
static TaskHandle_t xWatchdogTaskHandle;
static TaskHandle_t xHttpTaskHandle;
static TaskHandle_t xAggregatorTaskHandle;
static TaskHandle_t xSyncTaskHandle;
static TaskHandle_t xInvLowTaskHandle;
static TaskHandle_t xInvMedTaskHandle;
static TaskHandle_t xInvHighTaskHandle;

static StackType_t xLoggerStack[ configMINIMAL_STACK_SIZE ];
static StaticTask_t xLoggerTcb;

static volatile sig_atomic_t xShutdownRequested = 0;
static volatile BaseType_t xDashboardEnabled = pdFALSE;
static volatile BaseType_t xMonitorHangDemo = pdFALSE;
static volatile BaseType_t xHttpEnabled = pdFALSE;
static volatile BaseType_t xSlowConsumer = pdFALSE;
static volatile BaseType_t xMonitorPrioDemo = pdFALSE;
static volatile BaseType_t xInvDemoRunning = pdFALSE;
static volatile int iInvPhase = 0;
static volatile int iInvDtMs[ 2 ];
static volatile TickType_t xPrioDemoDeadline = 0;
static volatile TickType_t xWatchdogBeat[ WD_COUNT ];
static volatile BaseType_t xWatchdogFlagged[ WD_COUNT ];
static const TickType_t xWatchdogTimeout[ WD_COUNT ] =
{
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS ),        /* temp   */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS ),        /* hum    */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS ),        /* monitor */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS ),        /* alarm  */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS + 3000 ), /* stats: su periodo ya es de 5 s */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS ),        /* logger */
    pdMS_TO_TICKS( WATCHDOG_TIMEOUT_MS )         /* http   */
};
static const char * const pcWatchdogNames[ WD_COUNT ] =
{
    "temp", "hum", "monitor", "alarm", "stats", "logger", "http"
};

static unsigned int uiBaseSeed = 0;

static long long llEpochMs( void )
{
    struct timespec xNow;

    ( void ) clock_gettime( CLOCK_REALTIME, &xNow );

    return ( ( long long ) xNow.tv_sec * 1000LL ) +
           ( ( long long ) xNow.tv_nsec / 1000000LL );
}

static void vWatchdogBeat( WatchdogId_t xId )
{
    xWatchdogBeat[ xId ] = xTaskGetTickCount();
}


static struct termios xSavedTermios;
static BaseType_t xTerminalSaved = pdFALSE;
static BaseType_t xStdinIsTty = pdFALSE;
static BaseType_t xStdoutIsTty = pdFALSE;
static BaseType_t xStdinClosed = pdFALSE;

static unsigned int uiSpikeSeed = 1;

static const char * pcHelpText =
    "teclas: [t]=temp alarma [h]=hum alarma [p]=pausa [c]=continua [r]=reset [d]=dashboard\n"
    "        [w]=vigia [i]=inversion [v]=prioridad [k]=presion [s]=tareas [q]=salir [?]=ayuda";

static const char * pcHelpDashboard =
    "[t] alarma  [p] pausa  [c] sigue  [r] reset  [d] lineas  [w] vigia\n"
    "        [i] inversion  [v] prioridad  [k] presion  [s] tareas  [q] salir";

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

static void vPrintLine( const char * pcLine )
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
static void vReportEvent( const char * pcFormat,
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
        size_t xLength = strlen( pcBuffer );

        if( xLength >= PRINT_BUFFER_SIZE )
        {
            xLength = PRINT_BUFFER_SIZE - 1;
        }

        ( void ) xMessageBufferSend( xEventMessage, pcBuffer, xLength, 0 );
    }

    if( xDashboardEnabled == pdFALSE )
    {
        vPrintLine( pcBuffer );
    }
}

static void vShutdown( void )
{
    xSemaphoreTake( xPrintMutex, portMAX_DELAY );

    if( xDashboardEnabled != pdFALSE )
    {
        printf( "\x1b[2J\x1b[H" );
    }

    printf( "[%4lu s] saliendo...\n",
            ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ ) );
    fflush( stdout );
    xSemaphoreGive( xPrintMutex );

    vRestoreTerminal();
    _exit( 0 );
}

static void vFormatUptime( char * pcBuffer,
                           size_t xBufferSize )
{
    unsigned long ulSeconds = ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ );

    ( void ) snprintf( pcBuffer, xBufferSize, "%02lu:%02lu:%02lu",
                       ulSeconds / 3600UL,
                       ( ulSeconds / 60UL ) % 60UL,
                       ulSeconds % 60UL );
}

static const SensorConfig_t * pxGetSensorConfig( SensorId_t xId )
{
    return ( xId == SENSOR_TEMPERATURE ) ? &xTempSensor : &xHumSensor;
}

static BaseType_t xQueueForcedReading( const SensorConfig_t * pxConfig,
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
            ( void ) snprintf( xSystemState.pcLastEvent, sizeof( xSystemState.pcLastEvent ),
                               "!! ALARMA activa: umbral superado (alarmas totales: %lu) !!",
                               ulAlarmTotal );
            xSemaphoreGive( xStateMutex );

            xEventGroupSetBits( xEventGroup, ALARM_EVENT_BIT );
            xTaskNotifyGive( xStatsTaskHandle );

            if( xDashboardEnabled == pdFALSE )
            {
                vPrintFormat( "!! ALARMA activa: umbral superado (alarmas totales: %lu) !!",
                              ulAlarmTotal );
            }

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

static void vInvLowTask( void * pvParameters )
{
    SemaphoreHandle_t xLock;
    volatile int iWork = 0;
    int i;

    ( void ) pvParameters;

    for( ; ; )
    {
        ( void ) xEventGroupWaitBits( xInvEvents, INV_LOW_GO,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        xLock = ( iInvPhase == 1 ) ? xInvBinary : xInvMutex;
        ( void ) xSemaphoreTake( xLock, portMAX_DELAY );
        ( void ) xEventGroupSetBits( xInvEvents, INV_HELD );

        /* Trabajo en bucle con yield: la tarea sigue "lista" mientras
         * sostiene el lock (clave para la demo de inversion). */
        for( i = 0; i < INV_WORK_ITERS; i++ )
        {
            iWork = i;
            taskYIELD();
        }

        xSemaphoreGive( xLock );
    }

    ( void ) iWork;
}

static void vInvMedTask( void * pvParameters )
{
    TickType_t xStart;

    ( void ) pvParameters;

    for( ; ; )
    {
        ( void ) xEventGroupWaitBits( xInvEvents, INV_MED_GO,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        xStart = xTaskGetTickCount();

        /* Ocupa la CPU ~1 s; con prioridad 2 hunde a la BAJA (1). */
        while( ( xTaskGetTickCount() - xStart ) < pdMS_TO_TICKS( INV_MED_MS ) )
        {
            volatile int iSpin;

            for( iSpin = 0; iSpin < 500; iSpin++ )
            {
            }

            taskYIELD();
        }

        ( void ) xEventGroupSetBits( xInvEvents, INV_MED_DONE );
    }
}

static void vInvHighTask( void * pvParameters )
{
    SemaphoreHandle_t xLock;
    TickType_t xStart;
    TickType_t xEnd;

    ( void ) pvParameters;

    for( ; ; )
    {
        ( void ) xEventGroupWaitBits( xInvEvents, INV_HIGH_GO,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        xLock = ( iInvPhase == 1 ) ? xInvBinary : xInvMutex;
        xStart = xTaskGetTickCount();
        ( void ) xSemaphoreTake( xLock, portMAX_DELAY );
        xEnd = xTaskGetTickCount();

        iInvDtMs[ iInvPhase - 1 ] = ( int ) (
            ( ( unsigned long ) ( xEnd - xStart ) * 1000UL ) /
            ( unsigned long ) configTICK_RATE_HZ );

        xSemaphoreGive( xLock );
        ( void ) xEventGroupSetBits( xInvEvents, INV_HIGH_DONE );
    }
}

static void vInversionDemoTask( void * pvParameters )
{
    int iPhase;

    ( void ) pvParameters;

    for( iPhase = 1; iPhase <= 2; iPhase++ )
    {
        iInvPhase = iPhase;
        ( void ) xEventGroupClearBits( xInvEvents, INV_ALL_CLEAR );

        ( void ) xEventGroupSetBits( xInvEvents, INV_LOW_GO );
        ( void ) xEventGroupWaitBits( xInvEvents, INV_HELD,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        ( void ) xEventGroupSetBits( xInvEvents, INV_MED_GO );
        vTaskDelay( pdMS_TO_TICKS( 30 ) );

        ( void ) xEventGroupSetBits( xInvEvents, INV_HIGH_GO );
        ( void ) xEventGroupWaitBits( xInvEvents, INV_HIGH_DONE,
                                      pdTRUE, pdTRUE, portMAX_DELAY );
        ( void ) xEventGroupWaitBits( xInvEvents, INV_MED_DONE,
                                      pdTRUE, pdTRUE, portMAX_DELAY );
    }

    vReportEvent( "inversion: semaforo binario (sin herencia) -> la ALTA espero %d ms",
                  iInvDtMs[ 0 ] );
    vReportEvent( "inversion: mutex (herencia de prioridades) -> la ALTA espero %d ms",
                  iInvDtMs[ 1 ] );

    iInvPhase = 0;
    xInvDemoRunning = pdFALSE;
    vTaskDelete( NULL );
}

static unsigned long ulBusyPercentX10( void )
{
    TaskStatus_t xStatus[ 16 ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;
    UBaseType_t uxIndex;
    unsigned long ulIdleTenths = 0;
    unsigned long ulSumTenths = 0;
    BaseType_t xFoundIdle = pdFALSE;

    uxCount = uxTaskGetSystemState( xStatus, 16, &ulTotal );

    if( ( uxCount == 0 ) || ( ulTotal == 0 ) )
    {
        return 0UL;
    }

    for( uxIndex = 0; uxIndex < uxCount; uxIndex++ )
    {
        unsigned long ulTenths = ( ( unsigned long ) xStatus[ uxIndex ].ulRunTimeCounter * 1000UL ) / ( unsigned long ) ulTotal;

        if( strcmp( xStatus[ uxIndex ].pcTaskName, "IDLE" ) == 0 )
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

static void vDashboardDraw( void )
{
    SystemState_t xSnapshot;
    TaskStatus_t xStatus[ 16 ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;
    UBaseType_t uxIndex;
    unsigned long ulIdleTenths = 0;
    unsigned long ulBusyTenths;
    unsigned long ulTenths;
    unsigned int uiTempPercent;
    unsigned int uiHumPercent;
    unsigned int uiQueuePercent;
    unsigned int uiHeapPercent;
    unsigned int uiHeapFreeKb;
    unsigned long ulQueueCount;
    char pcBar[ VALUE_BAR_WIDTH + 8 ];
    char pcUptime[ 16 ];
    const char * pcStateColor;
    const char * pcStateText;
    const char * pcValueColor;
    size_t xFreeHeap;
    size_t xTotalHeap = configTOTAL_HEAP_SIZE;
    char pcSpark[ SPARK_HISTORY + 1 ];

    xSemaphoreTake( xStateMutex, portMAX_DELAY );
    xSnapshot = xSystemState;
    xSemaphoreGive( xStateMutex );

    uxCount = uxTaskGetSystemState( xStatus, 16, &ulTotal );

    for( uxIndex = 0; uxIndex < uxCount; uxIndex++ )
    {
        if( ulTotal == 0 )
        {
            ulTenths = 0;
        }
        else
        {
            ulTenths = ( ( unsigned long ) xStatus[ uxIndex ].ulRunTimeCounter * 1000UL ) / ( unsigned long ) ulTotal;
        }

        if( strcmp( xStatus[ uxIndex ].pcTaskName, "IDLE" ) == 0 )
        {
            ulIdleTenths = ulTenths;
        }
    }

    ulBusyTenths = ( ulIdleTenths < 1000UL ) ? ( 1000UL - ulIdleTenths ) : 0UL;

    vFormatUptime( pcUptime, sizeof( pcUptime ) );

    if( xSnapshot.xAlarmActive != pdFALSE )
    {
        pcStateColor = COLOR_RED;
        pcStateText = "ALARMA";
    }
    else if( xSnapshot.xSensorsPaused != pdFALSE )
    {
        pcStateColor = COLOR_YELLOW;
        pcStateText = "PAUSA";
    }
    else
    {
        pcStateColor = COLOR_GREEN;
        pcStateText = "NORMAL";
    }

    uiTempPercent = uiClampPercent( xSnapshot.iLastTempValue, 50 );
    uiHumPercent = uiClampPercent( xSnapshot.iLastHumValue, 100 );
    pcValueColor = ( xSnapshot.iLastTempValue > TEMP_ALARM_THRESHOLD ) ? COLOR_RED : COLOR_GREEN;

    ulQueueCount = ( unsigned long ) uxQueueMessagesWaiting( xSensorQueue );
    uiQueuePercent = ( unsigned int ) ( ( ulQueueCount * 100UL ) / ( unsigned long ) SENSOR_QUEUE_LENGTH );

    xFreeHeap = xPortGetFreeHeapSize();
    uiHeapPercent = ( unsigned int ) ( ( xFreeHeap * 100U ) / ( size_t ) xTotalHeap );
    uiHeapFreeKb = ( unsigned int ) ( xFreeHeap / 1024U );

    xSemaphoreTake( xPrintMutex, portMAX_DELAY );

    printf( "\x1b[H\x1b[2J" );
    printf( COLOR_BOLD "=== Estacion de sensores FreeRTOS (puerto POSIX) ===" COLOR_RESET "\n\n" );
    printf( "  uptime %s     estado: %s%s" COLOR_RESET "     alarmas: %lu     watchdog: %s%s" COLOR_RESET "\n\n",
            pcUptime,
            pcStateColor,
            pcStateText,
            xSnapshot.ulAlarms,
            ( xSnapshot.xWatchdogActive != pdFALSE ) ? COLOR_RED : COLOR_GREEN,
            ( xSnapshot.xWatchdogActive != pdFALSE ) ? "ALERTA" : "ok" );

    vFormatBar( pcBar, sizeof( pcBar ), uiTempPercent, VALUE_BAR_WIDTH );
    printf( "  temperatura  [%s]  %s%d C" COLOR_RESET "\n", pcBar, pcValueColor, xSnapshot.iLastTempValue );

    vFormatBar( pcBar, sizeof( pcBar ), uiHumPercent, VALUE_BAR_WIDTH );
    pcValueColor = ( xSnapshot.iLastHumValue > HUM_ALARM_THRESHOLD ) ? COLOR_RED : COLOR_GREEN;
    printf( "  humedad      [%s]  %s%d %%" COLOR_RESET "\n", pcBar, pcValueColor, xSnapshot.iLastHumValue );

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiTempHistory, xSnapshot.uiTempHistNext,
                  SPARK_HISTORY, 15, 40 );
    printf( "  hist temp    %s\n", pcSpark );

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiHumHistory, xSnapshot.uiHumHistNext,
                  SPARK_HISTORY, 0, 100 );
    printf( "  hist hum     %s\n\n", pcSpark );

    printf( "\n  lecturas %lu     perdidas %lu     picos %lu\n",
            xSnapshot.ulReadings, xSnapshot.ulDropped, xSnapshot.ulSpikes );

    vFormatBar( pcBar, sizeof( pcBar ), uiQueuePercent, 16 );
    printf( "  cola [%s] %lu/%u", pcBar, ulQueueCount, ( unsigned int ) SENSOR_QUEUE_LENGTH );

    vFormatBar( pcBar, sizeof( pcBar ), uiHeapPercent, 16 );
    printf( "     heap_libre [%s] %u/%u KiB\n", pcBar, uiHeapFreeKb,
            ( unsigned int ) ( xTotalHeap / 1024U ) );

    printf( "\n  " COLOR_BOLD "CPU por tarea" COLOR_RESET "\n" );

    for( uxIndex = 0; uxIndex < uxCount; uxIndex++ )
    {
        if( ulTotal == 0 )
        {
            ulTenths = 0;
        }
        else
        {
            ulTenths = ( ( unsigned long ) xStatus[ uxIndex ].ulRunTimeCounter * 1000UL ) / ( unsigned long ) ulTotal;
        }

        vFormatBar( pcBar, sizeof( pcBar ), ( unsigned int ) ( ulTenths / 10UL ), CPU_BAR_WIDTH );
        printf( "   %-9s %lu.%lu%%  %s\n",
                xStatus[ uxIndex ].pcTaskName,
                ulTenths / 10UL,
                ulTenths % 10UL,
                pcBar );
    }

    printf( "\n   ocupado: %lu.%lu%%\n", ulBusyTenths / 10UL, ulBusyTenths % 10UL );
    printf( "\n  " COLOR_YELLOW "ultimo evento:" COLOR_RESET " %s\n", xSnapshot.pcLastEvent );
    printf( "  " COLOR_BOLD "%s" COLOR_RESET "\n", pcHelpDashboard );
    fflush( stdout );

    xSemaphoreGive( xPrintMutex );
}

static void vRefreshCpuState( void )
{
    TaskStatus_t xStatus[ 16 ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;
    UBaseType_t uxIndex;
    unsigned long ulTemp = 0UL;
    unsigned long ulHum = 0UL;
    unsigned long ulMonitor = 0UL;
    unsigned long ulAlarm = 0UL;
    unsigned long ulTenths;

    uxCount = uxTaskGetSystemState( xStatus, 16, &ulTotal );

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

static void vCommandTask( void * pvParameters )
{
    const SensorConfig_t * pxConfig;
    char cKey;
    ssize_t lBytesRead;
    unsigned int uiCommandSeed = ( unsigned int ) time( NULL );

    ( void ) pvParameters;

    for( ; ; )
    {
        if( xShutdownRequested != 0 )
        {
            vShutdown();
        }

        if( ( xMonitorPrioDemo != pdFALSE ) &&
            ( xTaskGetTickCount() >= xPrioDemoDeadline ) )
        {
            vTaskPrioritySet( xMonitorTaskHandle, PRIORITY_MONITOR );
            xMonitorPrioDemo = pdFALSE;
            vReportEvent( "prioridades: monitor restaurado a prioridad %d",
                          PRIORITY_MONITOR );
        }

        if( xStdinClosed == pdFALSE )
        {
            lBytesRead = read( STDIN_FILENO, &cKey, 1 );

            if( lBytesRead == 1 )
            {
                switch( cKey )
                {
                    case 't':
                    case 'h':
                        pxConfig = pxGetSensorConfig( ( cKey == 't' ) ? SENSOR_TEMPERATURE : SENSOR_HUMIDITY );

                        if( xQueueForcedReading( pxConfig, &uiCommandSeed ) == pdPASS )
                        {
                            vReportEvent( "manual: inyectada lectura de %s forzada",
                                          pxConfig->pcName );
                        }
                        else
                        {
                            xSemaphoreGive( xDropSemaphore );
                            vReportEvent( "manual: cola llena, lectura forzada descartada" );
                        }

                        break;

                    case 'r':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.ulAlarms = 0;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: contador de alarmas reiniciado" );
                        break;

                    case 'p':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.xSensorsPaused = pdTRUE;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: sensores pausados" );
                        break;

                    case 'c':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.xSensorsPaused = pdFALSE;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: sensores reanudados" );
                        break;

                    case 'w':
                        if( xMonitorHangDemo == pdFALSE )
                        {
                            vTaskSuspend( xMonitorTaskHandle );
                            xMonitorHangDemo = pdTRUE;
                            vReportEvent( "demo vigia: tarea monitor suspendida (la detectara el watchdog)" );
                        }
                        else
                        {
                            xMonitorHangDemo = pdFALSE;
                            vTaskResume( xMonitorTaskHandle );
                            vReportEvent( "demo vigia: tarea monitor reanudada" );
                        }
                        break;

                    case 'k':
                        xSlowConsumer = ( xSlowConsumer == pdFALSE ) ? pdTRUE : pdFALSE;
                        vReportEvent( "presion inversa: consumidor lento %s (%d s por lectura)",
                                      ( xSlowConsumer != pdFALSE ) ? "ACTIVADO" : "desactivado",
                                      SLOW_CONSUMER_MS / 1000 );
                        break;

                    case 'v':
                        if( xMonitorPrioDemo == pdFALSE )
                        {
                            vTaskPrioritySet( xMonitorTaskHandle, PRIORITY_STATS );
                            xMonitorPrioDemo = pdTRUE;
                            xPrioDemoDeadline = xTaskGetTickCount() +
                                                pdMS_TO_TICKS( PRIORITY_DEMO_MS );
                            vReportEvent( "prioridades: monitor bajado de %d a %d con vTaskPrioritySet (5 s)",
                                          PRIORITY_MONITOR, PRIORITY_STATS );
                        }
                        else
                        {
                            vTaskPrioritySet( xMonitorTaskHandle, PRIORITY_MONITOR );
                            xMonitorPrioDemo = pdFALSE;
                            vReportEvent( "prioridades: monitor restaurado a prioridad %d",
                                          PRIORITY_MONITOR );
                        }
                        break;

                    case 'i':
                        if( xInvDemoRunning != pdFALSE )
                        {
                            vReportEvent( "inversion: la demo ya esta en curso" );
                        }
                        else
                        {
                            TaskHandle_t xDemoHandle;
                            BaseType_t xCreated;

                            xCreated = xTaskCreate( vInversionDemoTask, "invdemo",
                                                    configMINIMAL_STACK_SIZE, NULL,
                                                    PRIORITY_ALARM, &xDemoHandle );

                            if( xCreated == pdPASS )
                            {
                                xInvDemoRunning = pdTRUE;
                                vReportEvent( "inversion: demo iniciada (semforo binario vs mutex, ~2 s)" );
                            }
                            else
                            {
                                vReportEvent( "inversion: no se pudo crear la tarea de demo" );
                            }
                        }
                        break;

                    case 's':
                    {
                        static char pcTaskListBuffer[ 2048 ];
                        FILE * pxTasksFile;

                        vTaskList( pcTaskListBuffer );
                        pxTasksFile = fopen( "tasks.txt", "w" );

                        if( pxTasksFile != NULL )
                        {
                            ( void ) fputs( pcTaskListBuffer, pxTasksFile );
                            fclose( pxTasksFile );
                            vReportEvent( "tareas: volcado en tasks.txt (vTaskList)" );

                            if( xDashboardEnabled == pdFALSE )
                            {
                                xSemaphoreTake( xPrintMutex, portMAX_DELAY );
                                ( void ) printf( "%s", pcTaskListBuffer );
                                fflush( stdout );
                                xSemaphoreGive( xPrintMutex );
                            }
                        }
                        else
                        {
                            vReportEvent( "tareas: no se pudo escribir tasks.txt" );
                        }

                        break;
                    }


                    case 'd':
                        if( xStdoutIsTty == pdFALSE )
                        {
                            vReportEvent( "dashboard no disponible sin terminal" );
                            break;
                        }

                        xDashboardEnabled = ( xDashboardEnabled == pdFALSE ) ? pdTRUE : pdFALSE;

                        xSemaphoreTake( xPrintMutex, portMAX_DELAY );
                        printf( "\x1b[2J\x1b[H" );
                        fflush( stdout );
                        xSemaphoreGive( xPrintMutex );

                        xTaskNotifyGive( xStatsTaskHandle );
                        vReportEvent( "manual: modo %s activado",
                                      ( xDashboardEnabled != pdFALSE ) ? "dashboard" : "linea" );
                        break;

                    case 'q':
                        vShutdown();
                        break;

                    case '?':
                        if( xDashboardEnabled == pdFALSE )
                        {
                            vPrintLine( pcHelpText );
                        }

                        break;

                    default:
                        break;
                }
            }
            else if( lBytesRead == 0 )
            {
                if( xStdinIsTty == pdFALSE )
                {
                    xStdinClosed = pdTRUE;
                    vReportEvent( "stdin cerrado: comandos desactivados (senal para salir)" );
                }
            }
            else if( errno != EINTR )
            {
                xStdinClosed = pdTRUE;
                vReportEvent( "error leyendo stdin: comandos desactivados" );
            }
        }

        vTaskDelay( pdMS_TO_TICKS( COMMAND_POLL_MS ) );
    }
}

static void vLoggerTask( void * pvParameters )
{
    ReadingRecord_t xRecord;
    char pcMessage[ PRINT_BUFFER_SIZE ];
    size_t xBytes;
    FILE * pxReadings;
    FILE * pxEvents;
    unsigned long ulReadingsBytes = 0UL;

    ( void ) pvParameters;

    pxReadings = fopen( CSV_READINGS_PATH, "w" );
    pxEvents = fopen( CSV_EVENTS_PATH, "w" );

    if( ( pxReadings == NULL ) || ( pxEvents == NULL ) )
    {
        if( pxReadings != NULL )
        {
            fclose( pxReadings );
        }

        if( pxEvents != NULL )
        {
            fclose( pxEvents );
        }

        pxReadings = NULL;
        pxEvents = NULL;
        vReportEvent( "logger: no se pudo abrir los CSV, registro desactivado" );
    }
    else
    {
        ( void ) fprintf( pxReadings, "epoch_ms,sensor,valor,secuencia,alarma\n" );
        ( void ) fprintf( pxEvents, "epoch_ms,evento\n" );
        fflush( pxReadings );
        fflush( pxEvents );
        vReportEvent( "logger: CSV activo (%s y %s)", CSV_READINGS_PATH, CSV_EVENTS_PATH );
    }

    for( ; ; )
    {
        xBytes = xStreamBufferReceive( xReadingStream, &xRecord, sizeof( xRecord ),
                                        pdMS_TO_TICKS( 500 ) );
        vWatchdogBeat( WD_LOGGER );

        if( ( xBytes == sizeof( xRecord ) ) && ( pxReadings != NULL ) )
        {
            const SensorConfig_t * pxConfig = pxGetSensorConfig( xRecord.xId );
            int iWritten;

            iWritten = fprintf( pxReadings, "%lld,%s,%d,%lu,%d\n",
                                xRecord.llEpochMs,
                                pxConfig->pcName,
                                xRecord.iValue,
                                xRecord.ulSequence,
                                ( xRecord.xIsAlarm != pdFALSE ) ? 1 : 0 );

            if( iWritten > 0 )
            {
                ulReadingsBytes += ( unsigned long ) iWritten;
            }

            fflush( pxReadings );

            if( ulReadingsBytes >= CSV_ROTATE_BYTES )
            {
                /* Rotacion: el fichero actual pasa a readings.1.csv. */
                fclose( pxReadings );
                ( void ) rename( CSV_READINGS_PATH, "readings.1.csv" );
                pxReadings = fopen( CSV_READINGS_PATH, "w" );

                if( pxReadings != NULL )
                {
                    ulReadingsBytes = ( unsigned long ) fprintf(
                        pxReadings, "epoch_ms,sensor,valor,secuencia,alarma\n" );
                    fflush( pxReadings );
                    vReportEvent( "logger: readings.csv rotado (>64 KiB, respaldo en readings.1.csv)" );
                }
                else
                {
                    vReportEvent( "logger: rotacion fallo, CSV de lecturas desactivado" );
                }
            }
        }

        for( ; ; )
        {
            xBytes = xMessageBufferReceive( xEventMessage, pcMessage,
                                             sizeof( pcMessage ) - 1U, 0 );

            if( xBytes == 0U )
            {
                break;
            }

            pcMessage[ xBytes ] = '\0';

            if( pxEvents != NULL )
            {
                ( void ) fprintf( pxEvents, "%lld,\"%s\"\n", llEpochMs(), pcMessage );
                fflush( pxEvents );
            }
        }
    }
}

static void vWatchdogTask( void * pvParameters )
{
    TickType_t xNow;
    unsigned int uiIndex;
    BaseType_t xAnyOverdue;
    BaseType_t xPaused;

    ( void ) pvParameters;

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( WATCHDOG_CHECK_MS ) );

        xNow = xTaskGetTickCount();

        xSemaphoreTake( xStateMutex, portMAX_DELAY );
        xPaused = xSystemState.xSensorsPaused;
        xSemaphoreGive( xStateMutex );

        xAnyOverdue = pdFALSE;

        for( uiIndex = 0U; uiIndex < ( unsigned int ) WD_COUNT; uiIndex++ )
        {
            TickType_t xBeat = xWatchdogBeat[ uiIndex ];
            BaseType_t xOverdue;

            if( ( uiIndex == ( unsigned int ) WD_MONITOR ) && ( xPaused != pdFALSE ) )
            {
                /* Con los sensores en pausa el monitor no recibe lecturas:
                 * su espera en la cola es legitima, no una colgada. */
                xOverdue = pdFALSE;
            }
            else if( ( uiIndex == ( unsigned int ) WD_HTTP ) && ( xHttpEnabled == pdFALSE ) )
            {
                xOverdue = pdFALSE;
            }
            else
            {
                xOverdue = ( ( xNow - xBeat ) > xWatchdogTimeout[ uiIndex ] ) ? pdTRUE : pdFALSE;
            }

            if( ( xOverdue != pdFALSE ) && ( xWatchdogFlagged[ uiIndex ] == pdFALSE ) )
            {
                xWatchdogFlagged[ uiIndex ] = pdTRUE;

                xSemaphoreTake( xStateMutex, portMAX_DELAY );
                xSystemState.ulWatchdogFails++;
                xSemaphoreGive( xStateMutex );

                vReportEvent( "WATCHDOG: tarea '%s' sin latido desde hace %u s",
                              pcWatchdogNames[ uiIndex ],
                              ( unsigned int ) ( ( xNow - xBeat ) / configTICK_RATE_HZ ) );
            }
            else if( ( xOverdue == pdFALSE ) && ( xWatchdogFlagged[ uiIndex ] != pdFALSE ) )
            {
                xWatchdogFlagged[ uiIndex ] = pdFALSE;
                vReportEvent( "WATCHDOG: tarea '%s' recupero el latido", pcWatchdogNames[ uiIndex ] );
            }

            if( xOverdue != pdFALSE )
            {
                xAnyOverdue = pdTRUE;
            }
        }

        xSemaphoreTake( xStateMutex, portMAX_DELAY );
        xSystemState.xWatchdogActive = xAnyOverdue;
        xSemaphoreGive( xStateMutex );
    }
}

static void vHttpHandleClient( int iClient )
{
    static char pcBody[ 2048 ];
    char pcHeader[ 256 ];
    char pcRequest[ 512 ];
    char pcEvent[ EVENT_BUFFER_SIZE ];
    char pcUptime[ 16 ];
    SystemState_t xSnapshot;
    unsigned long ulBusy;
    size_t uiIndex;
    int iBodyLength;
    int iHeaderLength;
    int xWantJson;
    const char * pcEstado;
    const char * pcEstadoCss;
    const char * pcContentType;
    const char * pcWdText;
    const char * pcWdCss;

    memset( pcRequest, 0, sizeof( pcRequest ) );
    ( void ) recv( iClient, pcRequest, sizeof( pcRequest ) - 1, MSG_DONTWAIT );

    xWantJson = ( strncmp( pcRequest, "GET /metrics", 12 ) == 0 ) ? 1 : 0;

    xSemaphoreTake( xStateMutex, portMAX_DELAY );
    xSnapshot = xSystemState;
    xSemaphoreGive( xStateMutex );

    for( uiIndex = 0U; uiIndex < sizeof( pcEvent ) - 1U; uiIndex++ )
    {
        char cChar = xSnapshot.pcLastEvent[ uiIndex ];

        if( cChar == '\0' )
        {
            break;
        }

        pcEvent[ uiIndex ] = ( cChar == '"' ) ? '\'' : cChar;
    }

    pcEvent[ uiIndex ] = '\0';

    ulBusy = ulBusyPercentX10();
    vFormatUptime( pcUptime, sizeof( pcUptime ) );

    if( xSnapshot.xAlarmActive != pdFALSE )
    {
        pcEstado = "alarma";
        pcEstadoCss = "bad";
    }
    else if( xSnapshot.xSensorsPaused != pdFALSE )
    {
        pcEstado = "pausa";
        pcEstadoCss = "warn";
    }
    else
    {
        pcEstado = "normal";
        pcEstadoCss = "good";
    }

    if( xSnapshot.xWatchdogActive != pdFALSE )
    {
        pcWdText = "alerta";
        pcWdCss = "bad";
    }
    else
    {
        pcWdText = "ok";
        pcWdCss = "good";
    }

    if( xWantJson != 0 )
    {
        iBodyLength = snprintf( pcBody, sizeof( pcBody ),
                                "{\"uptime_s\":%lu,\"estado\":\"%s\",\"alarmas\":%lu,"
                                "\"lecturas\":%lu,\"perdidas\":%lu,\"picos\":%lu,"
                                "\"log_perdidas\":%lu,\"cola\":%u,\"heap_libre\":%u,"
                                "\"watchdog\":\"%s\",\"watchdog_fallos\":%lu,"
                                "\"cpu_ocupado_pct\":%lu.%lu,"
                                "\"cpu_pct\":{\"temp\":%lu.%lu,\"hum\":%lu.%lu,"
                                "\"monitor\":%lu.%lu,\"alarm\":%lu.%lu},"
                                "\"temp\":%d,\"hum\":%d,"
                                "\"ultimo_evento\":\"%s\"}",
                                ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ ),
                                pcEstado,
                                xSnapshot.ulAlarms,
                                xSnapshot.ulReadings,
                                xSnapshot.ulDropped,
                                xSnapshot.ulSpikes,
                                xSnapshot.ulLogDrops,
                                ( unsigned int ) uxQueueMessagesWaiting( xSensorQueue ),
                                ( unsigned int ) xPortGetFreeHeapSize(),
                                pcWdText,
                                xSnapshot.ulWatchdogFails,
                                ulBusy / 10UL,
                                ulBusy % 10UL,
                                xSnapshot.ulCpuTempX10 / 10UL,
                                xSnapshot.ulCpuTempX10 % 10UL,
                                xSnapshot.ulCpuHumX10 / 10UL,
                                xSnapshot.ulCpuHumX10 % 10UL,
                                xSnapshot.ulCpuMonitorX10 / 10UL,
                                xSnapshot.ulCpuMonitorX10 % 10UL,
                                xSnapshot.ulCpuAlarmX10 / 10UL,
                                xSnapshot.ulCpuAlarmX10 % 10UL,
                                xSnapshot.iLastTempValue,
                                xSnapshot.iLastHumValue,
                                pcEvent );
        pcContentType = "application/json; charset=utf-8";
    }
    else
    {
        iBodyLength = snprintf( pcBody, sizeof( pcBody ),
                                "<!doctype html>\n<html lang=\"es\">\n<head>\n"
                                "<meta charset=\"utf-8\">\n"
                                "<meta http-equiv=\"refresh\" content=\"2\">\n"
                                "<title>Estacion FreeRTOS</title>\n"
                                "<style>body{font-family:monospace;background:#0d1117;color:#c9d6d4;margin:2rem}"
                                "h1{font-size:1.1rem}table{border-collapse:collapse}"
                                "td,th{border:1px solid #30363d;padding:.35rem .7rem;text-align:left}"
                                ".bad{color:#f85149}.good{color:#3fb950}.warn{color:#e3b341}</style>\n"
                                "</head>\n<body>\n"
                                "<h1>Estacion de sensores FreeRTOS</h1>\n"
                                "<p>estado: <b class=\"%s\">%s</b> &nbsp; uptime: %s &nbsp; "
                                "alarmas: %lu &nbsp; watchdog: <b class=\"%s\">%s</b></p>\n"
                                "<table>\n"
                                "<tr><th>temperatura</th><td>%d C</td><th>humedad</th><td>%d %%</td></tr>\n"
                                "<tr><th>lecturas</th><td>%lu</td><th>perdidas</th><td>%lu</td></tr>\n"
                                "<tr><th>cola</th><td>%u/%u</td><th>heap libre</th><td>%u KiB</td></tr>\n"
                                "<tr><th>cpu sistema</th><td>%lu.%lu%%</td>"
                                "<th>cpu monitor</th><td>%lu.%lu%%</td></tr>\n"
                                "<tr><th>picos</th><td>%lu</td><th>log_perdidas</th><td>%lu</td></tr>\n"
                                "</table>\n"
                                "<p>ultimo evento: %s</p>\n"
                                "<p>JSON: <a href=\"/metrics\">/metrics</a> &middot; "
                                "auto-recarga cada 2 s</p>\n"
                                "</body>\n</html>\n",
                                pcEstadoCss,
                                pcEstado,
                                pcUptime,
                                xSnapshot.ulAlarms,
                                pcWdCss,
                                pcWdText,
                                xSnapshot.iLastTempValue,
                                xSnapshot.iLastHumValue,
                                xSnapshot.ulReadings,
                                xSnapshot.ulDropped,
                                ( unsigned int ) uxQueueMessagesWaiting( xSensorQueue ),
                                ( unsigned int ) SENSOR_QUEUE_LENGTH,
                                ( unsigned int ) ( xPortGetFreeHeapSize() / 1024U ),
                                ulBusy / 10UL,
                                ulBusy % 10UL,
                                xSnapshot.ulCpuMonitorX10 / 10UL,
                                xSnapshot.ulCpuMonitorX10 % 10UL,
                                xSnapshot.ulSpikes,
                                xSnapshot.ulLogDrops,
                                pcEvent );
        pcContentType = "text/html; charset=utf-8";
    }

    if( ( iBodyLength <= 0 ) || ( iBodyLength >= ( int ) sizeof( pcBody ) ) )
    {
        iBodyLength = 0;
    }

    iHeaderLength = snprintf( pcHeader, sizeof( pcHeader ),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n\r\n",
                              pcContentType,
                              iBodyLength );

    if( ( iHeaderLength > 0 ) && ( iHeaderLength < ( int ) sizeof( pcHeader ) ) )
    {
        ssize_t lWritten;

        lWritten = write( iClient, pcHeader, ( size_t ) iHeaderLength );

        if( lWritten == iHeaderLength )
        {
            ( void ) write( iClient, pcBody, ( size_t ) iBodyLength );
        }
    }

    ( void ) shutdown( iClient, SHUT_RDWR );
    close( iClient );
}

static void vHttpTask( void * pvParameters )
{
    int iSock;
    int iClient;
    int iOne = 1;
    struct sockaddr_in xAddress;

    ( void ) pvParameters;

    iSock = socket( AF_INET, SOCK_STREAM, 0 );

    if( iSock < 0 )
    {
        vReportEvent( "http: socket() fallo (errno=%d)", errno );
        vTaskDelete( NULL );
    }

    ( void ) setsockopt( iSock, SOL_SOCKET, SO_REUSEADDR, &iOne, sizeof( iOne ) );

    memset( &xAddress, 0, sizeof( xAddress ) );
    xAddress.sin_family = AF_INET;
    xAddress.sin_port = htons( HTTP_PORT );
    xAddress.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if( bind( iSock, ( struct sockaddr * ) &xAddress, sizeof( xAddress ) ) < 0 )
    {
        vReportEvent( "http: bind 127.0.0.1:%d fallo (errno=%d), servidor desactivado",
                      HTTP_PORT, errno );
        close( iSock );
        vTaskDelete( NULL );
    }

    if( listen( iSock, 4 ) < 0 )
    {
        vReportEvent( "http: listen() fallo (errno=%d), servidor desactivado", errno );
        close( iSock );
        vTaskDelete( NULL );
    }

    ( void ) fcntl( iSock, F_SETFL, O_NONBLOCK );
    xHttpEnabled = pdTRUE;
    vReportEvent( "http: servidor JSON en http://127.0.0.1:%d (curl para verlo)", HTTP_PORT );

    for( ; ; )
    {
        iClient = accept( iSock, NULL, NULL );

        if( iClient >= 0 )
        {
            vHttpHandleClient( iClient );
        }

        vWatchdogBeat( WD_HTTP );
        vTaskDelay( pdMS_TO_TICKS( HTTP_POLL_MS ) );
    }
}

static void vSpikeTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;

    if( xQueueForcedReading( &xTempSensor, &uiSpikeSeed ) == pdPASS )
    {
        xSemaphoreTake( xStateMutex, portMAX_DELAY );
        xSystemState.ulSpikes++;
        xSemaphoreGive( xStateMutex );
    }
    else
    {
        xSemaphoreGive( xDropSemaphore );
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
    xDropSemaphore = xSemaphoreCreateCounting( SENSOR_QUEUE_LENGTH, 0 );
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
