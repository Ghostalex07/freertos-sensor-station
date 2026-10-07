/* CSV logging of the sensor station.
 *
 * vLoggerTask consumes two independent sources:
 *  - xReadingStream (stream buffer): sensor readings.
 *  - xEventMessage  (message buffer): vReportEvent() events.
 * readings.csv rotates (to readings.1.csv) past CSV_ROTATE_BYTES. */

#include <FreeRTOS.h>

#include <stdio.h>

#include <stream_buffer.h>
#include <message_buffer.h>

#include "app_shared.h"
#include "watchdog.h"
#include "logger_csv.h"

/* Logger with static allocation: own TCB and stack, no pvPortMalloc.
 * main.c references them when creating the task with xTaskCreateStatic(). */
StackType_t xLoggerStack[ configMINIMAL_STACK_SIZE ];
StaticTask_t xLoggerTcb;

void vLoggerTask( void * pvParameters )
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
        vReportEvent( "logger: could not open the CSV files, logging disabled" );
    }
    else
    {
        ( void ) fprintf( pxReadings, "epoch_ms,sensor,value,sequence,alarm\n" );
        ( void ) fprintf( pxEvents, "epoch_ms,event\n" );
        fflush( pxReadings );
        fflush( pxEvents );
        vReportEvent( "logger: CSV active (%s and %s)", CSV_READINGS_PATH, CSV_EVENTS_PATH );
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
                /* Rotation: the current file becomes readings.1.csv. */
                fclose( pxReadings );
                ( void ) rename( CSV_READINGS_PATH, "readings.1.csv" );
                pxReadings = fopen( CSV_READINGS_PATH, "w" );

                if( pxReadings != NULL )
                {
                    ulReadingsBytes = ( unsigned long ) fprintf(
                        pxReadings, "epoch_ms,sensor,value,sequence,alarm\n" );
                    fflush( pxReadings );
                    vReportEvent( "logger: readings.csv rotated (>64 KiB, backup in readings.1.csv)" );
                }
                else
                {
                    vReportEvent( "logger: rotation failed, readings CSV disabled" );
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
