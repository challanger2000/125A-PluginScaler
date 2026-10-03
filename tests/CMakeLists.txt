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
