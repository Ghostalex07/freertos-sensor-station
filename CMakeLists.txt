cmake_minimum_required(VERSION 3.15)
project(freertos_sensor_station C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(FREERTOS_HEAP "4" CACHE STRING "")
set(FREERTOS_PORT "GCC_POSIX" CACHE STRING "")

# Static-everything build: every queue, semaphore, event group, buffer,
# timer and task is created with its static API, so the application never
# calls pvPortMalloc (heap_free stays at configTOTAL_HEAP_SIZE).
option(SENSOR_STATION_STATIC
       "Create every FreeRTOS object and task with static allocation" OFF)

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Threads REQUIRED)

if(DEFINED FREERTOS_KERNEL_PATH)
    get_filename_component(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH}" ABSOLUTE)
    message(STATUS "Using local kernel: ${FREERTOS_KERNEL_PATH}")
    add_subdirectory("${FREERTOS_KERNEL_PATH}" freertos-kernel)
else()
    include(FetchContent)
    FetchContent_Declare(freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG        V11.3.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(freertos_kernel)

    # Supply-chain anchor: the tag must point at the exact expected commit.
    # If someone moves the V11.3.1 tag on GitHub, configuration fails instead
    # of downloading unreviewed code.
    set(FREERTOS_KERNEL_EXPECTED_SHA "3a22924e0a9ddbbc8b0758881c33b3422a5cc20d")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${freertos_kernel_SOURCE_DIR}"
            OUTPUT_VARIABLE FREERTOS_KERNEL_SHA
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE FREERTOS_KERNEL_SHA_RESULT
        )
        if(NOT FREERTOS_KERNEL_SHA_RESULT EQUAL 0)
            message(FATAL_ERROR "Could not read the FreeRTOS kernel SHA")
        endif()
        if(NOT FREERTOS_KERNEL_SHA STREQUAL FREERTOS_KERNEL_EXPECTED_SHA)
            message(FATAL_ERROR
                "FreeRTOS-Kernel GIT_TAG V11.3.1 points at ${FREERTOS_KERNEL_SHA}, "
                "expected ${FREERTOS_KERNEL_EXPECTED_SHA}")
        endif()
        message(STATUS "FreeRTOS-Kernel pinned at ${FREERTOS_KERNEL_SHA}")
    endif()
endif()

add_executable(sensor_station
    src/main.c
    src/logic.c
    src/watchdog.c
    src/logger_csv.c
    src/http_server.c
    src/dashboard.c
    src/demos.c
)
target_compile_options(sensor_station PRIVATE -Wall -Wextra)
target_compile_definitions(sensor_station PRIVATE _DEFAULT_SOURCE)
if(SENSOR_STATION_STATIC)
    target_compile_definitions(sensor_station PRIVATE SENSOR_STATION_STATIC=1)
endif()
target_link_libraries(sensor_station freertos_kernel freertos_config Threads::Threads)

enable_testing()
add_executable(logic_tests tests/test_logic.c src/logic.c)
target_compile_options(logic_tests PRIVATE -Wall -Wextra)
target_include_directories(logic_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME logic_tests COMMAND logic_tests)
