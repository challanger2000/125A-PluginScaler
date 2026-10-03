add_executable(PluginScalerHelper
    main.cpp
)

target_link_libraries(PluginScalerHelper PRIVATE
    PluginScalerCore
    PluginScalerIPC
    PluginScalerVST2Adapter
)

target_compile_features(PluginScalerHelper PRIVATE cxx_std_20)
pluginscaler_enable_warnings(PluginScalerHelper)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(PluginScalerHelper PROPERTIES OUTPUT_NAME "PluginScalerHelper-x64")
else()
    set_target_properties(PluginScalerHelper PROPERTIES OUTPUT_NAME "PluginScalerHelper-x86")
endif()
