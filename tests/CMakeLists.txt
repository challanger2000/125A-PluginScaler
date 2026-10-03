add_executable(PluginScalerBootstrapTests
    BootstrapTests.cpp
)

target_link_libraries(PluginScalerBootstrapTests PRIVATE
    PluginScalerCore
    PluginScalerIPC
    PluginScalerVST3Adapter
    PluginScalerVST2Adapter
)

target_compile_features(PluginScalerBootstrapTests PRIVATE cxx_std_20)
pluginscaler_enable_warnings(PluginScalerBootstrapTests)

add_test(NAME PluginScaler.Bootstrap COMMAND PluginScalerBootstrapTests)

add_library(MockVST2 SHARED MockVST2.cpp)
target_include_directories(MockVST2 PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(MockVST2 PRIVATE cxx_std_20)
pluginscaler_enable_warnings(MockVST2)
set_target_properties(MockVST2 PROPERTIES PREFIX "" OUTPUT_NAME "MockVST2")

add_test(
    NAME PluginScaler.VST2Probe
    COMMAND PluginScalerHelper --probe-vst2 $<TARGET_FILE:MockVST2>
)
set_tests_properties(PluginScaler.VST2Probe PROPERTIES
    PASS_REGULAR_EXPRESSION "loaded=1;opened=1;closed=1;entry=VSTPluginMain;effect=125A Mock VST2"
)
