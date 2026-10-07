#ifndef WATCHDOG_H
#define WATCHDOG_H

/* Vigia de latidos: cada tarea periodica "late" y esta tarea avisa si
 * alguna deja de hacerlo durante demasiado tiempo. */

#include "app_types.h"

/* Registra el latido de una tarea (se llama desde la propia tarea). */
void vWatchdogBeat( WatchdogId_t xId );

/* Tarea que revisa los latidos y reporta altas y recuperaciones. */
void vWatchdogTask( void * pvParameters );

#endif
