cmake_minimum_required(VERSION 3.15)
project(freertos_sensor_station C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(FREERTOS_HEAP "4" CACHE STRING "")
set(FREERTOS_PORT "GCC_POSIX" CACHE STRING "")

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Threads REQUIRED)

if(DEFINED FREERTOS_KERNEL_PATH)
    get_filename_component(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH}" ABSOLUTE)
    message(STATUS "Usando kernel local: ${FREERTOS_KERNEL_PATH}")
    add_subdirectory("${FREERTOS_KERNEL_PATH}" freertos-kernel)
else()
    include(FetchContent)
    FetchContent_Declare(freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG        V11.3.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(freertos_kernel)

    # Ancla de suministro: el tag debe apuntar exactamente al commit esperado.
    # Si alguien mueve el tag V11.3.1 en GitHub, la configuracion falla en vez
    # de descargar codigo no revisado.
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
            message(FATAL_ERROR "No se pudo leer el SHA del kernel FreeRTOS")
        endif()
        if(NOT FREERTOS_KERNEL_SHA STREQUAL FREERTOS_KERNEL_EXPECTED_SHA)
            message(FATAL_ERROR
                "FreeRTOS-Kernel GIT_TAG V11.3.1 apunta a ${FREERTOS_KERNEL_SHA}, "
                "se esperaba ${FREERTOS_KERNEL_EXPECTED_SHA}")
        endif()
        message(STATUS "FreeRTOS-Kernel anclado en ${FREERTOS_KERNEL_SHA}")
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
target_link_libraries(sensor_station freertos_kernel freertos_config Threads::Threads)

enable_testing()
add_executable(logic_tests tests/test_logic.c src/logic.c)
target_compile_options(logic_tests PRIVATE -Wall -Wextra)
target_include_directories(logic_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME logic_tests COMMAND logic_tests)
