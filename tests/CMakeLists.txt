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

add_library(MockVST2Synth SHARED MockVST2Synth.cpp)
target_include_directories(MockVST2Synth PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(MockVST2Synth PRIVATE cxx_std_20)
pluginscaler_enable_warnings(MockVST2Synth)
set_target_properties(MockVST2Synth PROPERTIES PREFIX "" OUTPUT_NAME "MockVST2Synth")

add_test(
    NAME PluginScaler.VST2Probe
    COMMAND PluginScalerHelper --probe-vst2 $<TARGET_FILE:MockVST2>
)
set_tests_properties(PluginScaler.VST2Probe PROPERTIES
    PASS_REGULAR_EXPRESSION "loaded=1;opened=1;closed=1;entry=VSTPluginMain;effect=125A Mock VST2"
    TIMEOUT 20
)

add_test(
    NAME PluginScaler.VST2AudioProbe
    COMMAND PluginScalerHelper --probe-vst2-audio $<TARGET_FILE:MockVST2>
)
set_tests_properties(PluginScaler.VST2AudioProbe PROPERTIES
    PASS_REGULAR_EXPRESSION "configured=1;mainsOn=1;processed=1;mainsOff=1;closed=1;outL=0.1450;outR=0.1650"
    TIMEOUT 20
)

add_executable(PluginScalerBridgeClient BridgeClient.cpp)
target_include_directories(PluginScalerBridgeClient PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(PluginScalerBridgeClient PRIVATE PluginScalerIPC)
target_compile_features(PluginScalerBridgeClient PRIVATE cxx_std_20)
pluginscaler_enable_warnings(PluginScalerBridgeClient)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_test(
        NAME PluginScaler.VST2SharedMemoryBridge
        COMMAND PluginScalerBridgeClient
            $<TARGET_FILE:PluginScalerHelper>
            $<TARGET_FILE:MockVST2>
    )
    set_tests_properties(PluginScaler.VST2SharedMemoryBridge PROPERTIES
        PASS_REGULAR_EXPRESSION "state=3;error=0;outL0=0.145;outR0=0.165;outL63=1.405;outR63=2.685"
        TIMEOUT 20
    )
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(PluginScalerVST2ProxyHost VST2ProxyHost.cpp)
    target_include_directories(PluginScalerVST2ProxyHost PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(PluginScalerVST2ProxyHost PRIVATE cxx_std_20)
    pluginscaler_enable_warnings(PluginScalerVST2ProxyHost)

    add_executable(PluginScalerVST2SynthProxyHost VST2SynthProxyHost.cpp)
    target_include_directories(PluginScalerVST2SynthProxyHost PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(PluginScalerVST2SynthProxyHost PRIVATE cxx_std_20)
    pluginscaler_enable_warnings(PluginScalerVST2SynthProxyHost)
endif()
