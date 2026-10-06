add_executable(PluginScalerJBridgeProbe WIN32
    main.cpp
)

target_compile_features(PluginScalerJBridgeProbe PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridgeProbe PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridgeProbe PRIVATE user32)
