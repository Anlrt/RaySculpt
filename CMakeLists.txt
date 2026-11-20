cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAUSTIC_VERSION LIMIT_COUNT 1)
string(REGEX REPLACE "-dev$" "" CAUSTIC_NUMERIC_VERSION "${CAUSTIC_VERSION}")
project(CausticCADCore VERSION ${CAUSTIC_NUMERIC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(caustic_core
    src/geometry.cpp
    src/target_renderer.cpp
    src/preflight.cpp
    src/optics.cpp
    src/height_surface.cpp
    src/optimizer.cpp
    src/surface_mesh.cpp
    src/diagnostics.cpp
)
target_include_directories(caustic_core PUBLIC src)
if(MSVC)
    target_compile_options(caustic_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(caustic_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(caustic_cli src/main.cpp src/image_file.cpp src/plate_design.cpp src/photo_trace.cpp)
target_link_libraries(caustic_cli PRIVATE caustic_core)
target_compile_definitions(caustic_cli PRIVATE CAUSTIC_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools")
if(WIN32)
    target_link_libraries(caustic_cli PRIVATE ole32 windowscodecs)
endif()

add_executable(caustic_trace_demo examples/trace_plate.cpp)
target_link_libraries(caustic_trace_demo PRIVATE caustic_core)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name IN ITEMS geometry target_renderer preflight optics optimizer surface_mesh diagnostics)
        add_executable(${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(${test_name}_tests PRIVATE caustic_core)
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
    endforeach()
endif()
