cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        # Match the parent build; JUCE 7 uses APIs removed at a macOS 15 minimum.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    endif()
    project(GearmulatorLatencyHost LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(JUCE_MODULES_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../JUCE" JUCE)
endif()
add_executable(latency_host latency_host.cpp)
target_compile_definitions(latency_host PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
if(APPLE)
    target_compile_definitions(latency_host PRIVATE JUCE_PLUGINHOST_AU=1)
    target_link_libraries(latency_host PRIVATE "-framework AudioUnit" "-framework CoreAudioKit")
endif()
target_link_libraries(latency_host PRIVATE juce::juce_audio_processors juce::juce_audio_formats)

# Parent builds enable fast-math globally; the capture oracle must see NaN/Inf.
if(MSVC)
    target_compile_options(latency_host PRIVATE /fp:precise)
else()
    target_compile_options(latency_host PRIVATE -fno-fast-math)
endif()
set_property(TARGET latency_host PROPERTY FOLDER "Gearmulator/test")

# Raw executables need JUCE's per-directory header-only module annotations.
_juce_fixup_module_source_groups()
