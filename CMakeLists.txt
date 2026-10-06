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
endif()

add_executable(sensor_station main.c logic.c)
target_compile_options(sensor_station PRIVATE -Wall -Wextra)
target_compile_definitions(sensor_station PRIVATE _DEFAULT_SOURCE)
target_link_libraries(sensor_station freertos_kernel freertos_config Threads::Threads)

enable_testing()
add_executable(logic_tests tests/test_logic.c logic.c)
target_compile_options(logic_tests PRIVATE -Wall -Wextra)
target_include_directories(logic_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME logic_tests COMMAND logic_tests)
