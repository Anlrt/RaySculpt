cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAUSTIC_VERSION LIMIT_COUNT 1)
project(CausticCADCore VERSION ${CAUSTIC_VERSION} LANGUAGES CXX)

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
option(RAYSCULPT_BUILD_MESH_INDEX "Build indexed picking for original triangle meshes" OFF)
if(RAYSCULPT_BUILD_MESH_INDEX)
    add_library(caustic_mesh_index SHARED tools/mesh_index.cpp)
    target_link_libraries(caustic_mesh_index PRIVATE caustic_core)
endif()
option(RAYSCULPT_BUILD_SPECTRAL_FILTER "Build the OpenCV adapter for native photo numerics" OFF)
if(RAYSCULPT_BUILD_SPECTRAL_FILTER)
    find_package(OpenCV 4.10.0 EXACT CONFIG REQUIRED COMPONENTS core imgproc)
    add_library(caustic_spectral_filter SHARED tools/spectral_filter.cpp)
    target_include_directories(caustic_spectral_filter PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(caustic_spectral_filter PRIVATE ${OpenCV_LIBS})
    if(WIN32)
        add_custom_command(TARGET caustic_spectral_filter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:caustic_spectral_filter> $<TARGET_FILE_DIR:caustic_spectral_filter>
            COMMAND_EXPAND_LISTS)
    endif()
endif()
option(RAYSCULPT_BUILD_GPU_PREVIEW "Build the isolated Qt OpenGL viewport" OFF)
option(RAYSCULPT_BUILD_OCCT_EXPORT "Build the native OCCT plate exporter" OFF)
if(RAYSCULPT_BUILD_OCCT_EXPORT)
    find_package(OpenCASCADE CONFIG REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    add_executable(caustic_cad_export tools/cad_export.cpp)
    target_include_directories(caustic_cad_export PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(caustic_cad_export PRIVATE Qt6::Core TKDESTEP TKBool TKBO TKPrim TKTopAlgo TKBRep TKGeomBase TKG3d TKMath TKernel)
endif()
if(RAYSCULPT_BUILD_GPU_PREVIEW)
    find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
    add_executable(raysculpt_gpu_preview tools/gpu_preview.cpp)
    target_link_libraries(raysculpt_gpu_preview PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
    add_library(raysculpt_gpu_widget SHARED tools/gpu_preview.cpp)
    target_compile_definitions(raysculpt_gpu_widget PRIVATE CAUSTIC_GPU_WIDGET=1)
    target_link_libraries(raysculpt_gpu_widget PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
    add_executable(raysculpt_project_editor tools/project_editor.cpp)
    target_link_libraries(raysculpt_project_editor PRIVATE Qt6::Widgets)
endif()
if(BUILD_TESTING)
    add_executable(caustic_reference src/main.cpp src/image_file.cpp src/plate_design.cpp src/photo_trace.cpp)
    target_link_libraries(caustic_reference PRIVATE caustic_core)
    target_compile_definitions(caustic_reference PRIVATE CAUSTIC_REFERENCE_TRACE=1 CAUSTIC_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools")
    if(WIN32)
        target_link_libraries(caustic_reference PRIVATE ole32 windowscodecs)
    endif()
    foreach(test_name IN ITEMS geometry target_renderer preflight optics optimizer surface_mesh diagnostics)
        add_executable(${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(${test_name}_tests PRIVATE caustic_core)
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
    endforeach()
endif()
