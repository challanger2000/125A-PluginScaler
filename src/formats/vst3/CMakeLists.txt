add_library(PluginScalerVST3Adapter STATIC
    VST3Adapter.cpp
)

target_include_directories(PluginScalerVST3Adapter PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(PluginScalerVST3Adapter PUBLIC
    PluginScalerCore
)

target_compile_features(PluginScalerVST3Adapter PUBLIC cxx_std_20)
pluginscaler_enable_warnings(PluginScalerVST3Adapter)

if(PLUGINSCALER_FETCH_VST3_SDK)
    target_compile_definitions(PluginScalerVST3Adapter PUBLIC PLUGINSCALER_HAS_VST3_SDK=1)
    target_include_directories(PluginScalerVST3Adapter SYSTEM PUBLIC
        "${vst3sdk_SOURCE_DIR}"
    )
endif()
