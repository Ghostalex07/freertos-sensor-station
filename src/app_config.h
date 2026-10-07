#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* APPLICATION constants of the sensor station.
 * Only demo #defines live here: periods, thresholds, CSV paths,
 * priorities, buffer sizes and event bits.
 * The kernel configuration (FreeRTOSConfig.h) is a different matter and
 * is not touched from here. */

#define SENSOR_QUEUE_LENGTH    8
/* 16 tasks in steady state + slack for the demos (inv, invdemo...) :
 * uxTaskGetSystemState returns 0 if the array does not fit, which would
 * break the CPU metrics. */
#define TASK_STATUS_MAX        32
/* Capacity of the drop counter: with the slow consumer demo (k) the
 * 8-drop-per-5-s-window limit is exceeded. */
#define DROP_COUNT_MAX         1024
#define TEMP_ALARM_THRESHOLD   40
#define HUM_ALARM_THRESHOLD    85
#define TEMP_PERIOD_MS         700
#define HUM_PERIOD_MS          1100
#define ALARM_HOLD_MS          3000
#define STATS_PERIOD_MS        5000
#define SPIKE_PERIOD_MS        10000
#define PAUSE_POLL_MS          100
#define COMMAND_POLL_MS        50
#define ALARM_EVENT_BIT        ( 1 << 0 )

#define DASHBOARD_PERIOD_MS    500
#define WATCHDOG_CHECK_MS      500
#define WATCHDOG_TIMEOUT_MS    5000
#define HTTP_PORT              8080
#define HTTP_POLL_MS           50
#define STREAM_LENGTH          ( 16 * 64 )
#define MESSAGE_BUFFER_LENGTH  1024
#define SPARK_HISTORY          32
#define CSV_READINGS_PATH      "readings.csv"
#define CSV_EVENTS_PATH        "events.csv"

#define FORCED_OVERSHOOT       5
#define FORCED_JITTER          10
#define PRINT_BUFFER_SIZE      160
#define EVENT_BUFFER_SIZE      96
#define CPU_BAR_WIDTH          24
#define VALUE_BAR_WIDTH        28

#define SLOW_CONSUMER_MS       2000
#define PRIORITY_DEMO_MS       5000
#define CSV_ROTATE_BYTES       ( 64 * 1024 )
#define INV_WORK_ITERS         5000
#define INV_MED_MS             1000

#define SYNC_TEMP_BIT          ( 1 << 0 )
#define SYNC_HUM_BIT           ( 1 << 1 )

#define INV_LOW_GO             ( 1 << 0 )
#define INV_MED_GO             ( 1 << 1 )
#define INV_HIGH_GO            ( 1 << 2 )
#define INV_HELD               ( 1 << 3 )
#define INV_MED_DONE           ( 1 << 4 )
#define INV_HIGH_DONE          ( 1 << 5 )
#define INV_ALL_CLEAR          ( INV_HELD | INV_MED_DONE | INV_HIGH_DONE )

#define PRIORITY_ALARM         4
#define PRIORITY_MONITOR       3
#define PRIORITY_SENSOR        2
#define PRIORITY_COMMAND       2
#define PRIORITY_LOGGER        2
#define PRIORITY_STATS         1
#define PRIORITY_WATCHDOG      1
#define PRIORITY_HTTP          1

#define COLOR_RESET            "\x1b[0m"
#define COLOR_BOLD             "\x1b[1m"
#define COLOR_RED              "\x1b[31m"
#define COLOR_GREEN            "\x1b[32m"
#define COLOR_YELLOW           "\x1b[33m"

#endif
