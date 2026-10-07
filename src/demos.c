/* Tarea de comandos (teclado) y demos interactivas de la estacion.
 *
 * vCommandTask traduce cada tecla en una accion sobre el estado global:
 * pausas, alarmas forzadas y arranque de las demos de prioridades (v),
 * inversion de prioridades (i), presion inversa (k) y vigia (w). */

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
    "teclas: [t]=temp alarma [h]=hum alarma [p]=pausa [c]=continua [r]=reset [d]=dashboard\n"
    "        [w]=vigia [i]=inversion [v]=prioridad [k]=presion [s]=tareas [q]=salir [?]=ayuda";

/* Estado de las demos interactivas (teclas v, i, ...): privado de este
 * fichero, solo lo tocan vCommandTask y las tareas de inversion. */
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

        /* Trabajo en bucle con yield: la tarea sigue "lista" mientras
         * sostiene el lock (clave para la demo de inversion). */
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

    vReportEvent( "inversion: semaforo binario (sin herencia) -> la ALTA espero %d ms",
                  iInvDtMs[ 0 ] );
    vReportEvent( "inversion: mutex (herencia de prioridades) -> la ALTA espero %d ms",
                  iInvDtMs[ 1 ] );

    iInvPhase = 0;
    xInvDemoRunning = pdFALSE;
    vTaskDelete( NULL );
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
                        /* Nunca vTaskSuspend: si monitor se suspendiera con
                         * xPrintMutex/xStateMutex colgaria todo el sistema. */
                        xMonitorHangDemo = ( xMonitorHangDemo == pdFALSE ) ? pdTRUE : pdFALSE;
                        vReportEvent( "demo vigia: monitor %s (el watchdog %s)",
                                      ( xMonitorHangDemo != pdFALSE ) ? "detenido sin latido" : "reanudado",
                                      ( xMonitorHangDemo != pdFALSE ) ? "lo detectara" : "confirmara el latido" );
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
