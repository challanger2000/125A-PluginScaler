add_executable(PluginScalerJBridgeProbe WIN32
    main.cpp
)

target_compile_features(PluginScalerJBridgeProbe PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridgeProbe PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridgeProbe PRIVATE user32)


add_executable(PluginScalerJBridge150 WIN32
    scaler150.cpp
)

target_compile_features(PluginScalerJBridge150 PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridge150 PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridge150 PRIVATE user32)


add_executable(PluginScalerJBridgeCaptureProbe WIN32
    capture_probe.cpp
)

target_compile_features(PluginScalerJBridgeCaptureProbe PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridgeCaptureProbe PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridgeCaptureProbe PRIVATE user32 gdi32)


add_executable(PluginScalerJBridge150Viewer WIN32
    viewer150.cpp
)

target_compile_features(PluginScalerJBridge150Viewer PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridge150Viewer PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridge150Viewer PRIVATE user32 gdi32)


add_executable(PluginScalerJBridgeMultiInstanceProbe WIN32
    instance_probe.cpp
)

target_compile_features(PluginScalerJBridgeMultiInstanceProbe PRIVATE cxx_std_20)
target_compile_definitions(PluginScalerJBridgeMultiInstanceProbe PRIVATE UNICODE _UNICODE)
target_link_libraries(PluginScalerJBridgeMultiInstanceProbe PRIVATE user32)
