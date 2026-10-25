add_library(PluginScalerCore STATIC
    PluginDescriptor.cpp
    CompatibilityProfile.cpp
)

target_include_directories(PluginScalerCore PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_compile_features(PluginScalerCore PUBLIC cxx_std_20)
pluginscaler_enable_warnings(PluginScalerCore)
