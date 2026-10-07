/* Embedded HTTP server: serves the HTML dashboard and the metrics as
 * JSON (GET /metrics). All I/O is non-blocking with timeouts so a slow
 * client cannot hang the task. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <FreeRTOS.h>
#include <task.h>

#include "app_shared.h"
#include "watchdog.h"
#include "http_server.h"

/* Sends everything requested, retrying partial writes.
 * MSG_NOSIGNAL avoids SIGPIPE if the client closes early. */
static void vSendAll( int iSock,
                      const char * pcData,
                      int iLength )
{
    int iSent = 0;

    while( iSent < iLength )
    {
        ssize_t lWritten = send( iSock, pcData + iSent,
                                 ( size_t ) ( iLength - iSent ), MSG_NOSIGNAL );

        if( lWritten < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }

            break;
        }
        else if( lWritten == 0 )
        {
            break;
        }

        iSent += ( int ) lWritten;
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
    const char * pcState;
    const char * pcStateCss;
    const char * pcContentType;
    const char * pcWdText;
    const char * pcWdCss;

    memset( pcRequest, 0, sizeof( pcRequest ) );

    /* recv return checked: if it fails or the client closes without
     * data, the request stays empty and the HTML page is served. */
    if( recv( iClient, pcRequest, sizeof( pcRequest ) - 1, MSG_DONTWAIT ) <= 0 )
    {
        pcRequest[ 0 ] = '\0';
    }

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
        pcState = "alarm";
        pcStateCss = "bad";
    }
    else if( xSnapshot.xSensorsPaused != pdFALSE )
    {
        pcState = "paused";
        pcStateCss = "warn";
    }
    else
    {
        pcState = "normal";
        pcStateCss = "good";
    }

    if( xSnapshot.xWatchdogActive != pdFALSE )
    {
        pcWdText = "alert";
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
                                "{\"uptime_s\":%lu,\"state\":\"%s\",\"alarms\":%lu,"
                                "\"readings\":%lu,\"dropped\":%lu,\"spikes\":%lu,"
                                "\"isr_events\":%lu,\"max_age_ms\":%lu,"
                                "\"log_dropped\":%lu,\"queue\":%u,\"heap_free\":%u,"
                                "\"watchdog\":\"%s\",\"watchdog_fails\":%lu,"
                                "\"cpu_busy_pct\":%lu.%lu,"
                                "\"cpu_pct\":{\"temp\":%lu.%lu,\"hum\":%lu.%lu,"
                                "\"monitor\":%lu.%lu,\"alarm\":%lu.%lu},"
                                "\"temp\":%d,\"hum\":%d,"
                                "\"last_event\":\"%s\"}",
                                ( unsigned long ) ( xTaskGetTickCount() / configTICK_RATE_HZ ),
                                pcState,
                                xSnapshot.ulAlarms,
                                xSnapshot.ulReadings,
                                xSnapshot.ulDropped,
                                xSnapshot.ulSpikes,
                                xSnapshot.ulIsrEvents,
                                xSnapshot.ulMaxAgeMs,
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
                                "<!doctype html>\n<html lang=\"en\">\n<head>\n"
                                "<meta charset=\"utf-8\">\n"
                                "<meta http-equiv=\"refresh\" content=\"2\">\n"
                                "<title>FreeRTOS station</title>\n"
                                "<style>body{font-family:monospace;background:#0d1117;color:#c9d6d4;margin:2rem}"
                                "h1{font-size:1.1rem}table{border-collapse:collapse}"
                                "td,th{border:1px solid #30363d;padding:.35rem .7rem;text-align:left}"
                                ".bad{color:#f85149}.good{color:#3fb950}.warn{color:#e3b341}</style>\n"
                                "</head>\n<body>\n"
                                "<h1>Sensor station FreeRTOS</h1>\n"
                                "<p>state: <b class=\"%s\">%s</b> &nbsp; uptime: %s &nbsp; "
                                "alarms: %lu &nbsp; watchdog: <b class=\"%s\">%s</b></p>\n"
                                "<table>\n"
                                "<tr><th>temperature</th><td>%d C</td><th>humidity</th><td>%d %%</td></tr>\n"
                                "<tr><th>readings</th><td>%lu</td><th>dropped</th><td>%lu</td></tr>\n"
                                "<tr><th>queue</th><td>%u/%u</td><th>free heap</th><td>%u KiB</td></tr>\n"
                                "<tr><th>cpu system</th><td>%lu.%lu%%</td>"
                                "<th>cpu monitor</th><td>%lu.%lu%%</td></tr>\n"
                                "<tr><th>spikes</th><td>%lu</td><th>log_dropped</th><td>%lu</td></tr>\n"
                                "<tr><th>ISR events</th><td>%lu</td><th>max age</th><td>%lu ms</td></tr>\n"
                                "</table>\n"
                                "<p>last event: %s</p>\n"
                                "<p>JSON: <a href=\"/metrics\">/metrics</a> &middot; "
                                "auto-refresh every 2 s</p>\n"
                                "</body>\n</html>\n",
                                pcStateCss,
                                pcState,
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
                                xSnapshot.ulIsrEvents,
                                xSnapshot.ulMaxAgeMs,
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
        vSendAll( iClient, pcHeader, iHeaderLength );
        vSendAll( iClient, pcBody, iBodyLength );
    }

    ( void ) shutdown( iClient, SHUT_RDWR );
    close( iClient );
}

void vHttpTask( void * pvParameters )
{
    int iSock;
    int iClient;
    int iOne = 1;
    struct sockaddr_in xAddress;

    ( void ) pvParameters;

    iSock = socket( AF_INET, SOCK_STREAM, 0 );

    if( iSock < 0 )
    {
        vReportEvent( "http: socket() failed (errno=%d)", errno );
        vTaskDelete( NULL );
    }

    ( void ) setsockopt( iSock, SOL_SOCKET, SO_REUSEADDR, &iOne, sizeof( iOne ) );

    memset( &xAddress, 0, sizeof( xAddress ) );
    xAddress.sin_family = AF_INET;
    xAddress.sin_port = htons( HTTP_PORT );
    xAddress.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if( bind( iSock, ( struct sockaddr * ) &xAddress, sizeof( xAddress ) ) < 0 )
    {
        vReportEvent( "http: bind 127.0.0.1:%d failed (errno=%d), server disabled",
                      HTTP_PORT, errno );
        close( iSock );
        vTaskDelete( NULL );
    }

    if( listen( iSock, 4 ) < 0 )
    {
        vReportEvent( "http: listen() failed (errno=%d), server disabled", errno );
        close( iSock );
        vTaskDelete( NULL );
    }

    ( void ) fcntl( iSock, F_SETFL, O_NONBLOCK );
    xHttpEnabled = pdTRUE;
    vReportEvent( "http: JSON server on http://127.0.0.1:%d (curl to see it)", HTTP_PORT );

    for( ; ; )
    {
        iClient = accept( iSock, NULL, NULL );

        if( iClient >= 0 )
        {
            struct timeval xTimeout = { 2, 0 };

            /* On Linux the accepted socket does not inherit O_NONBLOCK:
             * timeouts bound the I/O so a slow client cannot hang http. */
            ( void ) setsockopt( iClient, SOL_SOCKET, SO_RCVTIMEO,
                                 &xTimeout, sizeof( xTimeout ) );
            ( void ) setsockopt( iClient, SOL_SOCKET, SO_SNDTIMEO,
                                 &xTimeout, sizeof( xTimeout ) );

            vHttpHandleClient( iClient );
        }

        vWatchdogBeat( WD_HTTP );
        vTaskDelay( pdMS_TO_TICKS( HTTP_POLL_MS ) );
    }
}
