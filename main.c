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
    char pcLastEvent[ EVENT_BUFFER_SIZE ];
} SystemState_t;

static QueueHandle_t xSensorQueue;
static SemaphoreHandle_t xPrintMutex;
static SemaphoreHandle_t xStateMutex;
static SemaphoreHandle_t xAlarmSemaphore;
static SemaphoreHandle_t xDropSemaphore;
static EventGroupHandle_t xEventGroup;
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

static volatile sig_atomic_t xShutdownRequested = 0;
static volatile BaseType_t xDashboardEnabled = pdFALSE;
static volatile BaseType_t xMonitorHangDemo = pdFALSE;
static volatile BaseType_t xHttpEnabled = pdFALSE;
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
    "teclas: [t]=temp alarma [h]=hum alarma [p]=pausa [c]=continua [r]=reset [d]=dashboard [w]=vigia [q]=salir";

static const char * pcHelpDashboard =
    "[t] temp  [h] hum  [p] pausa  [c] sigue  [r] reset  [d] lineas  [w] vigia  [q] salir";

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

static void vFormatBar( char * pcBuffer,
                        size_t xBufferSize,
                        unsigned int uiPercent,
                        unsigned int uiWidth )
{
    unsigned int uiFilled;
    unsigned int uiIndex;

    if( uiPercent > 100U )
    {
        uiPercent = 100U;
    }

    if( uiWidth >= xBufferSize )
    {
        uiWidth = ( unsigned int ) ( xBufferSize - 1U );
    }

    uiFilled = ( uiPercent * uiWidth ) / 100U;

    for( uiIndex = 0U; uiIndex < uiFilled; uiIndex++ )
    {
        pcBuffer[ uiIndex ] = '#';
    }

    for( ; uiIndex < uiWidth; uiIndex++ )
    {
        pcBuffer[ uiIndex ] = '-';
    }

    pcBuffer[ uiIndex ] = '\0';
}

static unsigned int uiClampPercent( int iValue,
                                    int iMaximum )
{
    unsigned int uiPercent;

    if( iValue <= 0 )
    {
        return 0U;
    }

    if( iValue >= iMaximum )
    {
        return 100U;
    }

    uiPercent = ( unsigned int ) ( ( iValue * 100 ) / iMaximum );

    return uiPercent;
}

static int iRandomRange( int iMin,
                         int iMax,
                         unsigned int * puiSeed )
{
    return iMin + ( int ) ( rand_r( puiSeed ) % ( unsigned int ) ( iMax - iMin + 1 ) );
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
            xIsAlarm = ( xReading.iValue > pxConfig->iAlarmThreshold ) ? pdTRUE : pdFALSE;

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

static void vFormatSpark( char * pcBuffer,
                          size_t xBufferSize,
                          const int * piHistory,
                          unsigned int uiNext,
                          int iMinimum,
                          int iMaximum )
{
    static const char pcLevels[] = " .:-=+*#%@";
    unsigned int uiCount;
    unsigned int uiStart;
    unsigned int uiIndex;

    if( uiNext < SPARK_HISTORY )
    {
        uiCount = uiNext;
        uiStart = 0U;
    }
    else
    {
        uiCount = SPARK_HISTORY;
        uiStart = uiNext - SPARK_HISTORY;
    }

    if( ( uiCount == 0U ) || ( xBufferSize < 2U ) )
    {
        ( void ) snprintf( pcBuffer, xBufferSize, "(sin datos)" );
        return;
    }

    if( uiCount > ( xBufferSize - 1U ) )
    {
        uiCount = ( unsigned int ) ( xBufferSize - 1U );
    }

    for( uiIndex = 0U; uiIndex < uiCount; uiIndex++ )
    {
        int iValue = piHistory[ ( uiStart + uiIndex ) % SPARK_HISTORY ];
        int iLevel;

        if( iValue <= iMinimum )
        {
            iLevel = 0;
        }
        else if( iValue >= iMaximum )
        {
            iLevel = 9;
        }
        else
        {
            iLevel = ( ( iValue - iMinimum ) * 9 ) / ( iMaximum - iMinimum );
        }

        pcBuffer[ uiIndex ] = pcLevels[ iLevel ];
    }

    pcBuffer[ uiIndex ] = '\0';
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

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiTempHistory, xSnapshot.uiTempHistNext, 15, 40 );
    printf( "  hist temp    %s\n", pcSpark );

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiHumHistory, xSnapshot.uiHumHistNext, 0, 100 );
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

            ( void ) fprintf( pxReadings, "%lld,%s,%d,%lu,%d\n",
                              xRecord.llEpochMs,
                              pxConfig->pcName,
                              xRecord.iValue,
                              xRecord.ulSequence,
                              ( xRecord.xIsAlarm != pdFALSE ) ? 1 : 0 );
            fflush( pxReadings );
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
    static char pcBody[ 512 ];
    char pcHeader[ 256 ];
    char pcRequest[ 512 ];
    char pcEvent[ EVENT_BUFFER_SIZE ];
    SystemState_t xSnapshot;
    unsigned long ulBusy;
    size_t uiIndex;
    int iBodyLength;
    int iHeaderLength;
    const char * pcEstado;

    ( void ) recv( iClient, pcRequest, sizeof( pcRequest ), MSG_DONTWAIT );

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

    if( xSnapshot.xAlarmActive != pdFALSE )
    {
        pcEstado = "alarma";
    }
    else if( xSnapshot.xSensorsPaused != pdFALSE )
    {
        pcEstado = "pausa";
    }
    else
    {
        pcEstado = "normal";
    }

    iBodyLength = snprintf( pcBody, sizeof( pcBody ),
                            "{\"uptime_s\":%lu,\"estado\":\"%s\",\"alarmas\":%lu,"
                            "\"lecturas\":%lu,\"perdidas\":%lu,\"picos\":%lu,"
                            "\"log_perdidas\":%lu,\"cola\":%u,\"heap_libre\":%u,"
                            "\"watchdog\":\"%s\",\"watchdog_fallos\":%lu,"
                            "\"cpu_ocupado_pct\":%lu.%lu,\"temp\":%d,\"hum\":%d,"
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
                            ( xSnapshot.xWatchdogActive != pdFALSE ) ? "alerta" : "ok",
                            xSnapshot.ulWatchdogFails,
                            ulBusy / 10UL,
                            ulBusy % 10UL,
                            xSnapshot.iLastTempValue,
                            xSnapshot.iLastHumValue,
                            pcEvent );

    if( ( iBodyLength <= 0 ) || ( iBodyLength >= ( int ) sizeof( pcBody ) ) )
    {
        iBodyLength = 0;
    }

    iHeaderLength = snprintf( pcHeader, sizeof( pcHeader ),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=utf-8\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n\r\n",
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
    xPrintMutex = xSemaphoreCreateMutex();
    xStateMutex = xSemaphoreCreateMutex();
    xAlarmSemaphore = xSemaphoreCreateBinary();
    xDropSemaphore = xSemaphoreCreateCounting( SENSOR_QUEUE_LENGTH, 0 );
    xEventGroup = xEventGroupCreate();
    xReadingStream = xStreamBufferCreate( STREAM_LENGTH, 1 );
    xEventMessage = xMessageBufferCreate( MESSAGE_BUFFER_LENGTH );

    configASSERT( ( xSensorQueue != NULL ) && ( xPrintMutex != NULL ) &&
                  ( xStateMutex != NULL ) && ( xAlarmSemaphore != NULL ) &&
                  ( xDropSemaphore != NULL ) && ( xEventGroup != NULL ) &&
                  ( xReadingStream != NULL ) && ( xEventMessage != NULL ) );

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

    xResult = xTaskCreate( vLoggerTask, "logger", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_LOGGER, &xLoggerTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vWatchdogTask, "watchdog", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_WATCHDOG, &xWatchdogTaskHandle );
    configASSERT( xResult == pdPASS );

    xResult = xTaskCreate( vHttpTask, "http", configMINIMAL_STACK_SIZE,
                            NULL, PRIORITY_HTTP, &xHttpTaskHandle );
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
