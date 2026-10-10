add_library(PluginScalerVST2Host STATIC
    VST2LegacyHost.cpp
)

target_include_directories(PluginScalerVST2Host PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(PluginScalerVST2Host PUBLIC
    PluginScalerVST2Adapter
)

target_compile_features(PluginScalerVST2Host PUBLIC cxx_std_20)
pluginscaler_enable_warnings(PluginScalerVST2Host)
