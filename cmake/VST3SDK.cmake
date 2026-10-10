include(FetchContent)

# Steinberg VST3 SDK 3.8.1.
# Pinned to an exact upstream commit for reproducible builds.
set(PLUGINSCALER_VST3_SDK_COMMIT
    "3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96"
    CACHE STRING "Pinned Steinberg VST3 SDK commit")

if(PLUGINSCALER_FETCH_VST3_SDK)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG ${PLUGINSCALER_VST3_SDK_COMMIT}
        GIT_SHALLOW FALSE
        GIT_SUBMODULES_RECURSE TRUE
    )

    FetchContent_MakeAvailable(vst3sdk)

    message(STATUS "PluginScaler VST3 SDK commit: ${PLUGINSCALER_VST3_SDK_COMMIT}")
endif()
