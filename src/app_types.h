#ifndef APP_TYPES_H
#define APP_TYPES_H

/* Data types of the sensor station.
 * They are separated from the tasks because almost every module needs
 * them (sensors, global state, CSV records and watchdog ids). */

#include <FreeRTOS.h>

#include "app_config.h"

typedef enum
{
    SENSOR_TEMPERATURE = 0,
    SENSOR_HUMIDITY
} SensorId_t;

typedef struct
{
    SensorId_t xId;
    int iValue;
    unsigned long ulSequence;
    /* Tick (xTaskGetTickCount) at which the reading was created; the
     * monitor turns it into the end-to-end age (max_age_ms). */
    unsigned long ulBornTick;
} SensorReading_t;

typedef struct
{
    long long llEpochMs;
    SensorId_t xId;
    int iValue;
    unsigned long ulSequence;
    BaseType_t xIsAlarm;
} ReadingRecord_t;

typedef enum
{
    WD_TEMP = 0,
    WD_HUM,
    WD_MONITOR,
    WD_ALARM,
    WD_STATS,
    WD_LOGGER,
    WD_HTTP,
    WD_COUNT
} WatchdogId_t;

typedef struct
{
    SensorId_t xId;
    const char * pcName;
    const char * pcUnit;
    int iAlarmThreshold;
    TickType_t xPeriodTicks;
    int iMin;
    int iMax;
} SensorConfig_t;

typedef struct
{
    int iLastTempValue;
    int iLastHumValue;
    unsigned long ulReadings;
    unsigned long ulAlarms;
    unsigned long ulDropped;
    unsigned long ulSpikes;
    unsigned long ulIsrEvents;
    unsigned long ulMaxAgeMs;
    unsigned long ulLogDrops;
    unsigned long ulWatchdogFails;
    BaseType_t xAlarmActive;
    BaseType_t xSensorsPaused;
    BaseType_t xWatchdogActive;
    int aiTempHistory[ SPARK_HISTORY ];
    int aiHumHistory[ SPARK_HISTORY ];
    unsigned int uiTempHistNext;
    unsigned int uiHumHistNext;
    unsigned long ulCpuTempX10;
    unsigned long ulCpuHumX10;
    unsigned long ulCpuMonitorX10;
    unsigned long ulCpuAlarmX10;
    char pcLastEvent[ EVENT_BUFFER_SIZE ];
} SystemState_t;

#endif
