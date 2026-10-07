/* Servidor HTTP embebido: sirve el dashboard en HTML y las metricas en
 * JSON (GET /metrics). Todo el E/S es no bloqueante con timeouts para
 * que un cliente lento no pueda colgar la tarea. */

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

/* Envia todo lo indicado reintentando escrituras parciales.
 * MSG_NOSIGNAL evita SIGPIPE si el cliente cerra antes de tiempo. */
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
    const char * pcEstado;
    const char * pcEstadoCss;
    const char * pcContentType;
    const char * pcWdText;
    const char * pcWdCss;

    memset( pcRequest, 0, sizeof( pcRequest ) );

    /* Retorno de recv comprobado: si falla o el cliente cierra sin datos,
     * la peticion queda vacia y se sirve la pagina HTML por defecto. */
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
            struct timeval xTimeout = { 2, 0 };

            /* En Linux el socket aceptado no hereda O_NONBLOCK: timeouts
             * acotan la E/S para que un cliente lento no cuelgue http. */
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
