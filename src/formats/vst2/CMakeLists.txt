add_library(PluginScalerVST2Adapter STATIC
    VST2AdapterStub.cpp
)

target_include_directories(PluginScalerVST2Adapter PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(PluginScalerVST2Adapter PUBLIC PluginScalerCore)
target_compile_features(PluginScalerVST2Adapter PUBLIC cxx_std_20)
pluginscaler_enable_warnings(PluginScalerVST2Adapter)
