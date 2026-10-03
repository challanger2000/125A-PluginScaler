if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(PluginScalerVST2Proxy SHARED
        VST2Proxy.cpp
        PluginScalerVST2Proxy.def
    )

    target_include_directories(PluginScalerVST2Proxy PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )

    target_link_libraries(PluginScalerVST2Proxy PRIVATE
        PluginScalerIPC
    )

    target_compile_features(PluginScalerVST2Proxy PRIVATE cxx_std_20)
    pluginscaler_enable_warnings(PluginScalerVST2Proxy)

    set_target_properties(PluginScalerVST2Proxy PROPERTIES
        PREFIX ""
        OUTPUT_NAME "PluginScalerVST2Proxy"
    )
endif()
