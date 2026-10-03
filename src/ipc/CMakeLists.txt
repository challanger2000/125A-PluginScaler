add_library(PluginScalerIPC STATIC
    Protocol.cpp
)

target_include_directories(PluginScalerIPC PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(PluginScalerIPC PUBLIC cxx_std_20)
target_link_libraries(PluginScalerIPC PUBLIC PluginScalerCore)
pluginscaler_enable_warnings(PluginScalerIPC)
