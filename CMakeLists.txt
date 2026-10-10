cmake_minimum_required(VERSION 3.28)

project(PluginScaler
    VERSION 0.1.0
    DESCRIPTION "125A PluginScaler"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "125A PluginScaler bootstrap currently targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PLUGINSCALER_FETCH_VST3_SDK "Fetch the pinned Steinberg VST3 SDK" ON)
option(PLUGINSCALER_BUILD_TESTS "Build bootstrap tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(VST3SDK)

add_subdirectory(src/core)
add_subdirectory(src/ipc)
add_subdirectory(src/helper)
add_subdirectory(src/proxy)
add_subdirectory(src/formats/vst3)
add_subdirectory(src/formats/vst2)
add_subdirectory(src/host)
add_subdirectory(src/jbridge_probe)
add_subdirectory(src/jbridge_hook)

if(PLUGINSCALER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
