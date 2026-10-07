/* Terminal dashboard: redraws the whole screen with ANSI every time
 * vStatsTask asks for it. It only renders: the state is read from
 * xSystemState (under xStateMutex) and the bars/sparklines are built by
 * logic.c (vFormatBar, vFormatSpark, uiClampPercent). */

#include <stdio.h>

#include <FreeRTOS.h>
#include <task.h>

#include "logic.h"
#include "app_shared.h"
#include "dashboard.h"

static const char * pcHelpDashboard =
    "[t] alarm  [p] pause  [c] resume  [r] reset  [d] lines  [w] watchdog\n"
    "      [i] inversion  [v] priority  [k] backpressure  [f] isr  [y] deadlock\n"
    "      [s] tasks  [q] exit";

void vDashboardDraw( void )
{
    SystemState_t xSnapshot;
    TaskStatus_t xStatus[ TASK_STATUS_MAX ];
    configRUN_TIME_COUNTER_TYPE ulTotal = 0;
    UBaseType_t uxCount;
    UBaseType_t uxIndex;
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

    uxCount = uxTaskGetSystemState( xStatus, TASK_STATUS_MAX, &ulTotal );
    ulBusyTenths = ulBusyFromSample( xStatus, uxCount, ulTotal );

    vFormatUptime( pcUptime, sizeof( pcUptime ) );

    if( xSnapshot.xAlarmActive != pdFALSE )
    {
        pcStateColor = COLOR_RED;
        pcStateText = "ALARM";
    }
    else if( xSnapshot.xSensorsPaused != pdFALSE )
    {
        pcStateColor = COLOR_YELLOW;
        pcStateText = "PAUSED";
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
    printf( COLOR_BOLD "=== Sensor station FreeRTOS (POSIX port) ===" COLOR_RESET "\n\n" );
    printf( "  uptime %s     state: %s%s" COLOR_RESET "     alarms: %lu     watchdog: %s%s" COLOR_RESET "\n\n",
            pcUptime,
            pcStateColor,
            pcStateText,
            xSnapshot.ulAlarms,
            ( xSnapshot.xWatchdogActive != pdFALSE ) ? COLOR_RED : COLOR_GREEN,
            ( xSnapshot.xWatchdogActive != pdFALSE ) ? "ALERT" : "ok" );

    vFormatBar( pcBar, sizeof( pcBar ), uiTempPercent, VALUE_BAR_WIDTH );
    printf( "  temperature  [%s]  %s%d C" COLOR_RESET "\n", pcBar, pcValueColor, xSnapshot.iLastTempValue );

    vFormatBar( pcBar, sizeof( pcBar ), uiHumPercent, VALUE_BAR_WIDTH );
    pcValueColor = ( xSnapshot.iLastHumValue > HUM_ALARM_THRESHOLD ) ? COLOR_RED : COLOR_GREEN;
    printf( "  humidity     [%s]  %s%d %%" COLOR_RESET "\n", pcBar, pcValueColor, xSnapshot.iLastHumValue );

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiTempHistory, xSnapshot.uiTempHistNext,
                  SPARK_HISTORY, 15, 40 );
    printf( "  hist temp    %s\n", pcSpark );

    vFormatSpark( pcSpark, sizeof( pcSpark ), xSnapshot.aiHumHistory, xSnapshot.uiHumHistNext,
                  SPARK_HISTORY, 0, 100 );
    printf( "  hist hum     %s\n\n", pcSpark );

    printf( "\n  readings %lu     dropped %lu     spikes %lu\n",
            xSnapshot.ulReadings, xSnapshot.ulDropped, xSnapshot.ulSpikes );
    printf( "  isr_events %lu     max_age_ms %lu\n",
            xSnapshot.ulIsrEvents, xSnapshot.ulMaxAgeMs );

    vFormatBar( pcBar, sizeof( pcBar ), uiQueuePercent, 16 );
    printf( "  queue [%s] %lu/%u", pcBar, ulQueueCount, ( unsigned int ) SENSOR_QUEUE_LENGTH );

    vFormatBar( pcBar, sizeof( pcBar ), uiHeapPercent, 16 );
    printf( "     heap_free [%s] %u/%u KiB\n", pcBar, uiHeapFreeKb,
            ( unsigned int ) ( xTotalHeap / 1024U ) );

    printf( "\n  " COLOR_BOLD "CPU per task" COLOR_RESET "\n" );

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

    printf( "\n   busy: %lu.%lu%%\n", ulBusyTenths / 10UL, ulBusyTenths % 10UL );
    printf( "\n  " COLOR_YELLOW "last event:" COLOR_RESET " %s\n", xSnapshot.pcLastEvent );
    printf( "  " COLOR_BOLD "%s" COLOR_RESET "\n", pcHelpDashboard );
    fflush( stdout );

    xSemaphoreGive( xPrintMutex );
}
