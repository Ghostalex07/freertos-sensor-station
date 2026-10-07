/* Vigia de latidos (watchdog) de la estacion de sensores.
 *
 * Dos piezas: vWatchdogBeat() la llaman las propias tareas para decir
 * "sigo viva", y vWatchdogTask() compara cada latido con su timeout.
 * Tablas y estado son privados de este fichero. */

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#include "app_shared.h"
#include "watchdog.h"

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

void vWatchdogBeat( WatchdogId_t xId )
{
    xWatchdogBeat[ xId ] = xTaskGetTickCount();
}

void vWatchdogTask( void * pvParameters )
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
