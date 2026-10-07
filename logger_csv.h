#ifndef LOGGER_CSV_H
#define LOGGER_CSV_H

/* Tarea encargada de persistir lecturas y eventos en CSV. */

#include <FreeRTOS.h>

/* Pila y TCB estaticos de la tarea logger (definidos en logger_csv.c). */
extern StackType_t xLoggerStack[ configMINIMAL_STACK_SIZE ];
extern StaticTask_t xLoggerTcb;

void vLoggerTask( void * pvParameters );

#endif
