#ifndef LOGGER_CSV_H
#define LOGGER_CSV_H

/* Task in charge of persisting readings and events as CSV. */

#include <FreeRTOS.h>

/* Static stack and TCB of the logger task (defined in logger_csv.c). */
extern StackType_t xLoggerStack[ configMINIMAL_STACK_SIZE ];
extern StaticTask_t xLoggerTcb;

void vLoggerTask( void * pvParameters );

#endif
