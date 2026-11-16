cmake_minimum_required(VERSION 3.22)
project(125APluginScalerVST3Prototype LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This GUI prototype targets Windows only.")
endif()

add_executable(PluginScalerVST3Window WIN32 src/vst3_window_scaler.cpp)
target_compile_features(PluginScalerVST3Window PRIVATE cxx_std_20)
target_include_directories(PluginScalerVST3Window PRIVATE include)
target_compile_definitions(PluginScalerVST3Window PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PluginScalerVST3Window PRIVATE user32 Magnification)
set_property(TARGET PluginScalerVST3Window PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_testing()
add_executable(PluginScalerGeometryTests tests/geometry_tests.cpp)
target_compile_features(PluginScalerGeometryTests PRIVATE cxx_std_20)
target_include_directories(PluginScalerGeometryTests PRIVATE include)
set_property(TARGET PluginScalerGeometryTests PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME geometry_mapping COMMAND PluginScalerGeometryTests)

if(MSVC)
  target_compile_options(PluginScalerVST3Window PRIVATE /W4 /WX /permissive- /utf-8)
  target_compile_options(PluginScalerGeometryTests PRIVATE /W4 /WX /permissive-)
endif()
