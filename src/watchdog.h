#ifndef WATCHDOG_H
#define WATCHDOG_H

/* Heartbeat watchdog: every periodic task "beats" and this task warns
 * when one stops doing so for too long. */

#include "app_types.h"

/* Records the beat of a task (called from the task itself). */
void vWatchdogBeat( WatchdogId_t xId );

/* Task that checks the beats and reports failures and recoveries. */
void vWatchdogTask( void * pvParameters );

#endif
