add_library(PluginScalerProxyCore STATIC
    ProxySession.cpp
)

target_include_directories(PluginScalerProxyCore PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(PluginScalerProxyCore PUBLIC
    PluginScalerCore
    PluginScalerIPC
)

target_compile_features(PluginScalerProxyCore PUBLIC cxx_std_20)
pluginscaler_enable_warnings(PluginScalerProxyCore)
