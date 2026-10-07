/* Keyboard command task and interactive demos of the station.
 *
 * vCommandTask translates every key into an action on the global state:
 * pauses, forced alarms and the start of the priority demos (v),
 * priority inversion (i), backpressure (k) and watchdog (w). */

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <event_groups.h>

#include "app_shared.h"
#include "demos.h"

const char * pcHelpText =
    "keys: [t]=temp alarm [h]=hum alarm [p]=pause [c]=continue [r]=reset [d]=dashboard\n"
    "      [w]=watchdog [i]=inversion [v]=priority [k]=backpressure [f]=isr [y]=deadlock [s]=tasks [q]=exit [?]=help";

/* State of the interactive demos (v, i, ... keys): private to this file,
 * only vCommandTask and the inversion tasks touch it. */
volatile BaseType_t xMonitorPrioDemo = pdFALSE;
volatile BaseType_t xInvDemoRunning = pdFALSE;
volatile int iInvPhase = 0;
volatile int iInvDtMs[ 2 ];
volatile TickType_t xPrioDemoDeadline = 0;

static BaseType_t xStdinClosed = pdFALSE;

void vInvLowTask( void * pvParameters )
{
    SemaphoreHandle_t xLock;
    int i;

    ( void ) pvParameters;

    for( ; ; )
    {
        ( void ) xEventGroupWaitBits( xInvEvents, INV_LOW_GO,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        xLock = ( iInvPhase == 1 ) ? xInvBinary : xInvMutex;
        ( void ) xSemaphoreTake( xLock, portMAX_DELAY );
        ( void ) xEventGroupSetBits( xInvEvents, INV_HELD );

        /* Busy work with yield: the task stays "ready" while it holds
         * the lock (key to the inversion demo). */
        for( i = 0; i < INV_WORK_ITERS; i++ )
        {
            taskYIELD();
        }

        xSemaphoreGive( xLock );
    }
}

void vInvMedTask( void * pvParameters )
{
    TickType_t xStart;

    ( void ) pvParameters;

    for( ; ; )
    {
        ( void ) xEventGroupWaitBits( xInvEvents, INV_MED_GO,
                                      pdTRUE, pdTRUE, portMAX_DELAY );

        xStart = xTaskGetTickCount();

        /* Hogs the CPU for ~1 s; at priority 2 it sinks the LOW one (1). */
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

void vInvHighTask( void * pvParameters )
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

        if( ( iInvPhase >= 1 ) && ( iInvPhase <= 2 ) )
        {
            iInvDtMs[ iInvPhase - 1 ] = ( int ) (
                ( ( unsigned long ) ( xEnd - xStart ) * 1000UL ) /
                ( unsigned long ) configTICK_RATE_HZ );
        }

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

    vReportEvent( "inversion: binary semaphore (no inheritance) -> the HIGH task waited %d ms",
                  iInvDtMs[ 0 ] );
    vReportEvent( "inversion: mutex (priority inheritance) -> the HIGH task waited %d ms",
                  iInvDtMs[ 1 ] );

    iInvPhase = 0;
    xInvDemoRunning = pdFALSE;
    vTaskDelete( NULL );
}

/* --- demo 'y': recoverable AB/BA deadlock ---------------------------- */

static volatile BaseType_t xDeadlockDemoRunning = pdFALSE;
static int iDeadlockRemaining = 0;

/* Classic circular wait: dl1 grabs lock A then fights for B, dl2 grabs B
 * then fights for A. The second take ALWAYS has a timeout: that bounded
 * wait is what breaks the cycle (an unbounded take would hang forever).
 * dl2 starts holding later so dl1 is the one that times out first and
 * releases A, which unblocks dl2. */
static void vDeadlockRun( SemaphoreHandle_t xFirst,
                          SemaphoreHandle_t xSecond,
                          const char * pcName,
                          TickType_t xHoldDelay )
{
    ( void ) xSemaphoreTake( xFirst, portMAX_DELAY );
    vTaskDelay( xHoldDelay );

    if( xSemaphoreTake( xSecond, pdMS_TO_TICKS( DEADLOCK_TIMEOUT_MS ) ) == pdPASS )
    {
        vReportEvent( "deadlock: %s acquired both locks after the other task released",
                      pcName );
        ( void ) xSemaphoreGive( xSecond );
    }
    else
    {
        vReportEvent( "deadlock: %s timed out after %d ms waiting for the second lock (recovered)",
                      pcName, DEADLOCK_TIMEOUT_MS );
    }

    ( void ) xSemaphoreGive( xFirst );

    if( __atomic_sub_fetch( &iDeadlockRemaining, 1, __ATOMIC_RELAXED ) == 0 )
    {
        xDeadlockDemoRunning = pdFALSE;
    }

    vTaskDelete( NULL );
}

static void vDeadlockDl1( void * pvParameters )
{
    ( void ) pvParameters;
    vDeadlockRun( xDeadlockA, xDeadlockB, "dl1", pdMS_TO_TICKS( DEADLOCK_LOCK_TICKS ) );
}

static void vDeadlockDl2( void * pvParameters )
{
    ( void ) pvParameters;
    vDeadlockRun( xDeadlockB, xDeadlockA, "dl2", pdMS_TO_TICKS( DEADLOCK_LOCK2_TICKS ) );
}

void vCommandTask( void * pvParameters )
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
            vReportEvent( "priorities: monitor restored to priority %d",
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
                            vReportEvent( "manual: injected forced %s reading",
                                          pxConfig->pcName );
                        }
                        else
                        {
                            xSemaphoreGive( xDropSemaphore );
                            vReportEvent( "manual: queue full, forced reading dropped" );
                        }

                        break;

                    case 'r':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.ulAlarms = 0;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: alarm counter reset" );
                        break;

                    case 'p':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.xSensorsPaused = pdTRUE;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: sensors paused" );
                        break;

                    case 'c':
                        xSemaphoreTake( xStateMutex, portMAX_DELAY );
                        xSystemState.xSensorsPaused = pdFALSE;
                        xSemaphoreGive( xStateMutex );
                        vReportEvent( "manual: sensors resumed" );
                        break;

                    case 'w':
                        /* Never vTaskSuspend: if monitor suspended while
                         * holding xPrintMutex/xStateMutex the whole system
                         * would hang. */
                        xMonitorHangDemo = ( xMonitorHangDemo == pdFALSE ) ? pdTRUE : pdFALSE;
                        vReportEvent( "watchdog demo: monitor %s (the watchdog %s)",
                                      ( xMonitorHangDemo != pdFALSE ) ? "stopped without beating" : "resumed",
                                      ( xMonitorHangDemo != pdFALSE ) ? "will detect it" : "will confirm the beat" );
                        break;

                    case 'k':
                        xSlowConsumer = ( xSlowConsumer == pdFALSE ) ? pdTRUE : pdFALSE;
                        vReportEvent( "backpressure: slow consumer %s (%d s per reading)",
                                      ( xSlowConsumer != pdFALSE ) ? "ENABLED" : "disabled",
                                      SLOW_CONSUMER_MS / 1000 );
                        break;

                    case 'f':
                        /* Written from task context, read from the tick
                         * hook (ISR): volatile + the kernel queue do the
                         * rest. */
                        xIsrDemoEnabled = ( xIsrDemoEnabled == pdFALSE ) ? pdTRUE : pdFALSE;
                        vReportEvent( "isr: demo %s (1 event/s from vApplicationTickHook via xQueueSendFromISR)",
                                      ( xIsrDemoEnabled != pdFALSE ) ? "enabled" : "disabled" );
                        break;

                    case 'y':
                        if( xDeadlockDemoRunning != pdFALSE )
                        {
                            vReportEvent( "deadlock: demo already running" );
                        }
                        else
                        {
                            TaskHandle_t xDl1 = NULL;
                            TaskHandle_t xDl2 = NULL;
                            BaseType_t xCreated;

                            iDeadlockRemaining = 2;
                            xDeadlockDemoRunning = pdTRUE;
                            xCreated = xTaskCreate( vDeadlockDl1, "dl1", DEADLOCK_STACK_WORDS,
                                                    NULL, PRIORITY_ALARM, &xDl1 );

                            if( xCreated == pdPASS )
                            {
                                xCreated = xTaskCreate( vDeadlockDl2, "dl2", DEADLOCK_STACK_WORDS,
                                                        NULL, PRIORITY_ALARM, &xDl2 );
                            }

                            if( xCreated == pdPASS )
                            {
                                vReportEvent( "deadlock: demo started (AB/BA, the second take has a %d ms timeout)",
                                              DEADLOCK_TIMEOUT_MS );
                            }
                            else
                            {
                                if( xDl1 != NULL )
                                {
                                    vTaskDelete( xDl1 );
                                }

                                iDeadlockRemaining = 0;
                                xDeadlockDemoRunning = pdFALSE;
                                vReportEvent( "deadlock: could not create the demo tasks" );
                            }
                        }
                        break;

                    case 'v':
                        if( xMonitorPrioDemo == pdFALSE )
                        {
                            vTaskPrioritySet( xMonitorTaskHandle, PRIORITY_STATS );
                            xMonitorPrioDemo = pdTRUE;
                            xPrioDemoDeadline = xTaskGetTickCount() +
                                                pdMS_TO_TICKS( PRIORITY_DEMO_MS );
                            vReportEvent( "priorities: monitor lowered from %d to %d with vTaskPrioritySet (5 s)",
                                          PRIORITY_MONITOR, PRIORITY_STATS );
                        }
                        else
                        {
                            vTaskPrioritySet( xMonitorTaskHandle, PRIORITY_MONITOR );
                            xMonitorPrioDemo = pdFALSE;
                            vReportEvent( "priorities: monitor restored to priority %d",
                                          PRIORITY_MONITOR );
                        }
                        break;

                    case 'i':
                        if( xInvDemoRunning != pdFALSE )
                        {
                            vReportEvent( "inversion: the demo is already running" );
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
                                vReportEvent( "inversion: demo started (binary semaphore vs mutex, ~2 s)" );
                            }
                            else
                            {
                                vReportEvent( "inversion: could not create the demo task" );
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
                            vReportEvent( "tasks: dump written to tasks.txt (vTaskList)" );

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
                            vReportEvent( "tasks: could not write tasks.txt" );
                        }

                        break;
                    }

                    case 'd':
                        if( xStdoutIsTty == pdFALSE )
                        {
                            vReportEvent( "dashboard unavailable without a terminal" );
                            break;
                        }

                        xDashboardEnabled = ( xDashboardEnabled == pdFALSE ) ? pdTRUE : pdFALSE;

                        xSemaphoreTake( xPrintMutex, portMAX_DELAY );
                        printf( "\x1b[2J\x1b[H" );
                        fflush( stdout );
                        xSemaphoreGive( xPrintMutex );

                        xTaskNotifyGive( xStatsTaskHandle );
                        vReportEvent( "manual: %s mode enabled",
                                      ( xDashboardEnabled != pdFALSE ) ? "dashboard" : "line" );
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
                    vReportEvent( "stdin closed: commands disabled (signal to exit)" );
                }
            }
            else if( errno != EINTR )
            {
                xStdinClosed = pdTRUE;
                vReportEvent( "error reading stdin: commands disabled" );
            }
        }

        vTaskDelay( pdMS_TO_TICKS( COMMAND_POLL_MS ) );
    }
}
