#ifndef APP_TYPES_H
#define APP_TYPES_H

/* Tipos de datos de la estacion de sensores.
 * Se separan de las tareas porque casi todos los modulos los necesitan
 * (sensores, estado global, registros CSV y ids del watchdog). */

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
