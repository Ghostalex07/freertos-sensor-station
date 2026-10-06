#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>
#include <event_groups.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#define SENSOR_QUEUE_LENGTH    8
#define TEMP_ALARM_THRESHOLD   40
#define HUM_ALARM_THRESHOLD    85
#define ALARM_HOLD_MS          3000
#define STATS_PERIOD_MS        5000
#define SPIKE_PERIOD_MS        10000
#define ALARM_EVENT_BIT        ( 1 << 0 )

#define FORCED_OVERSHOOT       5
#define FORCED_JITTER          10
#define PRINT_BUFFER_SIZE      160

#define PRIORITY_ALARM         4
#define PRIORITY_MONITOR       3
#define PRIORITY_SENSOR        2
#define PRIORITY_COMMAND       2
#define PRIORITY_STATS         1

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
    SensorId_t xId;
    const char * pcName;
    const char * pcUnit;
    int iAlarmThreshold;
    TickType_t xPeriodTicks;
    int iMin;
    int iMax;
} SensorConfig_t;

static QueueHandle_t xSensorQueue;
static SemaphoreHandle_t xPrintMutex;
static SemaphoreHandle_t xStateMutex;
static SemaphoreHandle_t xAlarmSemaphore;
static EventGroupHandle_t xEventGroup;

static unsigned long ulReadingCount = 0;
static unsigned long ulAlarmCount = 0;

static TaskHandle_t xTempTaskHandle;
static TaskHandle_t xHumTaskHandle;
static TaskHandle_t xMonitorTaskHandle;
static TaskHandle_t xAlarmTaskHandle;
static TaskHandle_t xStatsTaskHandle;
static TaskHandle_t xCommandTaskHandle;

static struct termios xSavedTermios;

static const SensorConfig_t xTempSensor =
{
    .xId = SENSOR_TEMPERATURE,
    .pcName = "temperatura",
    .pcUnit = "C",
    .iAlarmThreshold = TEMP_ALARM_THRESHOLD,
    .xPeriodTicks = pdMS_TO_TICKS( 700 ),
    .iMin = 15,
    .iMax = 38
};

static const SensorConfig_t xHumSensor =
{
    .xId = SENSOR_HUMIDITY,
    .pcName = "humedad",
    .pcUnit = "%",
    .iAlarmThreshold = HUM_ALARM_THRESHOLD,
    .xPeriodTicks = pdMS_TO_TICKS( 1100 ),
    .iMin = 30,
    .iMax = 82
};

static void vRestoreTerminal( void )
{
    tcsetattr( STDIN_FILENO, TCSANOW, &xSavedTermios );
}

static void vEnterRawTerminal( void )
{
    struct termios xRaw;

    tcgetattr( STDIN_FILENO, &xSavedTermios );
    xRaw = xSavedTermios;
    xRaw.c_lflag &= ( tcflag_t ) ~( ICANON | ECHO );
    xRaw.c_cc[ VMIN ] = 0;
    xRaw.c_cc[ VTIME ] = 0;
    tcsetattr( STDIN_FILENO, TCSANOW, &xRaw );
    atexit( vRestoreTerminal );
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
    int iLength;

    va_start( xArgs, pcFormat );
    iLength = vsnprintf( pcBuffer, sizeof( pcBuffer ), pcFormat, xArgs );
    va_end( xArgs );

    if( iLength < 0 )
    {
        pcBuffer[ 0 ] = '\0';
    }

    vPrintLine( pcBuffer );
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

static BaseType_t xQueueForcedReading( const SensorConfig_t * pxConfig )
{
    SensorReading_t xReading;

    xReading.xId = pxConfig->xId;
    xReading.iValue = pxConfig->iAlarmThreshold + FORCED_OVERSHOOT + ( rand() % FORCED_JITTER );
    xReading.ulSequence = 0;

    return xQueueSend( xSensorQueue, &xReading, 0 );
}

static void vSensorTask( void * pvParameters )
{
    const SensorConfig_t * pxConfig = ( const SensorConfig_t * ) pvParameters;
    SensorReading_t xReading;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    unsigned int uiSeed = ( unsigned int ) ( ( unsigned long ) pvParameters ^ xLastWakeTime );

    for( ; ; )
    {
        xReading.xId = pxConfig->xId;
        xReading.iValue = iRandomRange( pxConfig->iMin, pxConfig->iMax, &uiSeed );

        xSemaphoreTake( xStateMutex, portMAX_DELAY );
        xReading.ulSequence = ++ulReadingCount;
        xSemaphoreGive( xStateMutex );

        if( xQueueSend( xSensorQueue, &xReading, 0 ) != pdPASS )
        {
            vPrintLine( "cola de sensores llena: lectura descartada" );
        }

        vTaskDelayUntil( &xLastWakeTime, pxConfig->xPeriodTicks );
    }
}

static void vMonitorTask( void * pvParameters )
{
    SensorReading_t xReading;
    const SensorConfig_t * pxConfig;
    BaseType_t xIsAlarm;

    ( void ) pvParameters;

    for( ; ; )
    {
        if( xQueueReceive( xSensorQueue, &xReading, portMAX_DELAY ) == pdPASS )
        {
            pxConfig = pxGetSensorConfig( xReading.xId );
            xIsAlarm = ( xReading.iValue > pxConfig->iAlarmThreshold ) ? pdTRUE : pdFALSE;

            vPrintFormat( "%-10s #%lu = %d %s%s",
                          pxConfig->pcName,
                          xReading.ulSequence,
                          xReading.iValue,
                          pxConfig->pcUnit,
                          ( xIsAlarm == pdTRUE ) ? "  <-- ALARMA" : "" );

            if( xIsAlarm == pdTRUE )
            {
                xSemaphoreGive( xAlarmSemaphore );
            }
        }
    }
}

static void vAlarmTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        if( xSemaphoreTake( xAlarmSemaphore, portMAX_DELAY ) == pdPASS )
        {
            xSemaphoreTake( xStateMutex, portMAX_DELAY );
            ulAlarmCount++;
            xSemaphoreGive( xStateMutex );

            xEventGroupSetBits( xEventGroup, ALARM_EVENT_BIT );

            vPrintFormat( "!! ALARMA activa: umbral superado (alarmas totales: %lu) !!",
                          ulAlarmCount );

            vTaskDelay( pdMS_TO_TICKS( ALARM_HOLD_MS ) );
            xEventGroupClearBits( xEventGroup, ALARM_EVENT_BIT );
        }
    }
}

static void vStatsTask( void * pvParameters )
{
    EventBits_t xBits;

    ( void ) pvParameters;

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( STATS_PERIOD_MS ) );

        xBits = xEventGroupGetBits( xEventGroup );

        vPrintFormat( "stats: lecturas=%lu alarmas=%lu cola=%u/%u heap_libre=%zu B tareas=%u eventos=%s",
                      ulReadingCount,
                      ulAlarmCount,
                      ( unsigned int ) uxQueueMessagesWaiting( xSensorQueue ),
                      ( unsigned int ) SENSOR_QUEUE_LENGTH,
                      xPortGetFreeHeapSize(),
                      ( unsigned int ) uxTaskGetNumberOfTasks(),
                      ( xBits & ALARM_EVENT_BIT ) ? "ALARMA" : "normal" );

        vPrintFormat( "stack minima libre (palabras): monitor=%u alarm=%u stats=%u",
                      ( unsigned int ) uxTaskGetStackHighWaterMark( xMonitorTaskHandle ),
                      ( unsigned int ) uxTaskGetStackHighWaterMark( xAlarmTaskHandle ),
                      ( unsigned int ) uxTaskGetStackHighWaterMark( xStatsTaskHandle ) );
    }
}

static void vCommandTask( void * pvParameters )
{
    const SensorConfig_t * pxConfig;
    char cKey;
    ssize_t lBytesRead;

    ( void ) pvParameters;

    for( ; ; )
    {
        lBytesRead = read( STDIN_FILENO, &cKey, 1 );

        if( lBytesRead == 1 )
        {
            switch( cKey )
            {
                case 't':
                case 'h':
                    pxConfig = pxGetSensorConfig( ( cKey == 't' ) ? SENSOR_TEMPERATURE : SENSOR_HUMIDITY );

                    if( xQueueForcedReading( pxConfig ) == pdPASS )
                    {
                        vPrintFormat( "manual: inyectada lectura de %s forzada",
                                      pxConfig->pcName );
                    }

                    break;

                case 'r':
                    xSemaphoreTake( xStateMutex, portMAX_DELAY );
                    ulAlarmCount = 0;
                    xSemaphoreGive( xStateMutex );
                    vPrintLine( "manual: contador de alarmas reiniciado" );
                    break;

                case 'p':
                    vTaskSuspend( xTempTaskHandle );
                    vTaskSuspend( xHumTaskHandle );
                    vPrintLine( "manual: sensores suspendidos (vTaskSuspend)" );
                    break;

                case 'c':
                    vTaskResume( xTempTaskHandle );
                    vTaskResume( xHumTaskHandle );
                    vPrintLine( "manual: sensores reanudados (vTaskResume)" );
                    break;

                case 'q':
                    vPrintLine( "saliendo..." );
                    vRestoreTerminal();
                    exit( 0 );
                    break;

                case '?':
                    vPrintLine( "teclas: [t]=temp alarma [h]=hum alarma [p]=pausa [c]=continua [r]=reset [q]=salir" );
                    break;

                default:
                    break;
            }
        }

        vTaskDelay( pdMS_TO_TICKS( 50 ) );
    }
}

static void vSpikeTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    ( void ) xQueueForcedReading( &xTempSensor );
}

void vApplicationStackOverflowHook( TaskHandle_t xTask,
                                    char * pcTaskName )
{
    ( void ) xTask;
    fprintf( stderr, "STACK OVERFLOW en tarea %s\n", pcTaskName );
    abort();
}

int main( void )
{
    TimerHandle_t xSpikeTimer;

    srand( ( unsigned int ) time( NULL ) );
    vEnterRawTerminal();

    printf( "=== Estacion de sensores FreeRTOS (puerto POSIX) ===\n" );
    printf( "teclas: [t]=temp alarma [h]=hum alarma [p]=pausa [c]=continua [r]=reset [q]=salir\n\n" );

    xSensorQueue = xQueueCreate( SENSOR_QUEUE_LENGTH, sizeof( SensorReading_t ) );
    xPrintMutex = xSemaphoreCreateMutex();
    xStateMutex = xSemaphoreCreateMutex();
    xAlarmSemaphore = xSemaphoreCreateBinary();
    xEventGroup = xEventGroupCreate();

    configASSERT( ( xSensorQueue != NULL ) && ( xPrintMutex != NULL ) &&
                  ( xStateMutex != NULL ) && ( xAlarmSemaphore != NULL ) &&
                  ( xEventGroup != NULL ) );

    xTaskCreate( vSensorTask, "temp", configMINIMAL_STACK_SIZE,
                 ( void * ) &xTempSensor, PRIORITY_SENSOR, &xTempTaskHandle );
    xTaskCreate( vSensorTask, "hum", configMINIMAL_STACK_SIZE,
                 ( void * ) &xHumSensor, PRIORITY_SENSOR, &xHumTaskHandle );
    xTaskCreate( vMonitorTask, "monitor", configMINIMAL_STACK_SIZE,
                 NULL, PRIORITY_MONITOR, &xMonitorTaskHandle );
    xTaskCreate( vAlarmTask, "alarm", configMINIMAL_STACK_SIZE,
                 NULL, PRIORITY_ALARM, &xAlarmTaskHandle );
    xTaskCreate( vStatsTask, "stats", configMINIMAL_STACK_SIZE,
                 NULL, PRIORITY_STATS, &xStatsTaskHandle );
    xTaskCreate( vCommandTask, "command", configMINIMAL_STACK_SIZE,
                 NULL, PRIORITY_COMMAND, &xCommandTaskHandle );

    xSpikeTimer = xTimerCreate( "spike", pdMS_TO_TICKS( SPIKE_PERIOD_MS ),
                                pdTRUE, NULL, vSpikeTimerCallback );
    configASSERT( xSpikeTimer != NULL );
    xTimerStart( xSpikeTimer, 0 );

    vTaskStartScheduler();

    printf( "Error: el scheduler no arranco\n" );
    return 1;
}
