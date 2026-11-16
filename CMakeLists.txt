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

# Native Windows capture smoke, using the actual viewer implementation.
add_executable(PluginScalerCaptureSmoke tests/capture_smoke.cpp)
target_compile_features(PluginScalerCaptureSmoke PRIVATE cxx_std_20)
target_include_directories(PluginScalerCaptureSmoke PRIVATE include)
target_compile_definitions(PluginScalerCaptureSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PluginScalerCaptureSmoke PRIVATE user32 gdi32 Magnification)
set_property(TARGET PluginScalerCaptureSmoke PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(PluginScalerCaptureSmoke PRIVATE /W4 /WX /permissive- /utf-8)
endif()

# Real 125A VST3 visual integration smoke with the real viewer/Win32 capture.
# The 125A product is built independently in a separate GitHub Actions job.
add_executable(PluginScalerRealVST3Smoke tests/real_vst3_gui_smoke.cpp)
target_compile_features(PluginScalerRealVST3Smoke PRIVATE cxx_std_20)
target_include_directories(PluginScalerRealVST3Smoke PRIVATE include)
target_compile_definitions(PluginScalerRealVST3Smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PluginScalerRealVST3Smoke PRIVATE user32 gdi32 Magnification)
set_property(TARGET PluginScalerRealVST3Smoke PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(PluginScalerRealVST3Smoke PRIVATE /W4 /WX /permissive- /utf-8)
endif()

# VST2 editor-host x64. Load arbitrary user DLLs into a disposable
# standalone Windows process, without a DAW and without a VST2 SDK.
add_executable(PluginScalerVST2Host tests/vst2_editor_host.cpp)
target_compile_features(PluginScalerVST2Host PRIVATE cxx_std_20)
target_include_directories(PluginScalerVST2Host PRIVATE include)
set_property(TARGET PluginScalerVST2Host PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(PluginScalerVST2Host PRIVATE /W4 /WX /permissive- /utf-8)
endif()

# CI-only genuine VST2-DLL fixture. Do not ship as a user plugin.
add_library(PluginScalerVST2Fixture SHARED tests/vst2_fixture.cpp)
target_compile_features(PluginScalerVST2Fixture PRIVATE cxx_std_20)
target_include_directories(PluginScalerVST2Fixture PRIVATE include)
set_property(TARGET PluginScalerVST2Fixture PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(PluginScalerVST2Fixture PRIVATE /W4 /WX /permissive- /utf-8)
endif()
