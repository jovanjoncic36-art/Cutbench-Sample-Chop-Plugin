cmake_minimum_required(VERSION 3.22)

# Must be set before project()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary (Apple Silicon + Intel)")

project(Cutbench VERSION 0.3.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use a local checkout if given, otherwise fetch a pinned release.
include(FetchContent)

set(CUTBENCH_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE checkout")
if(CUTBENCH_JUCE_DIR)
    add_subdirectory(${CUTBENCH_JUCE_DIR} JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(Cutbench
    COMPANY_NAME                "Cutbench"
    PRODUCT_NAME                "Cutbench"
    PLUGIN_MANUFACTURER_CODE    Cbnc
    PLUGIN_CODE                 Cb03
    FORMATS                     AU VST3 Standalone
    VST3_CATEGORIES             Instrument Sampler
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     TRUE)   # installs to ~/Library/Audio/Plug-Ins/VST3

target_sources(Cutbench PRIVATE
    Source/SampleData.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/WaveformView.cpp
    Source/PadGrid.cpp
    Source/StretchEngine.cpp)

target_compile_definitions(Cutbench PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Cutbench
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Independent pitch / time-stretch engine: Signalsmith Stretch + Signalsmith Linear (both header-only, MIT).
# Switch off with -DCUTBENCH_STRETCH=OFF; the plugin then runs with the "Vinyl" engine only.
option(CUTBENCH_STRETCH    "Enable the Signalsmith Stretch engine (Keep Length mode)" ON)
option(CUTBENCH_ACCELERATE "Use Apple Accelerate for the stretch engine's FFTs (macOS)" ON)

if(CUTBENCH_STRETCH)
    # The libraries are only fetched, not built: SOURCE_SUBDIR points to a folder that doesn't exist, so their
    # own CMake files are skipped. They are cloned to <build>/ss_deps/<name>, so that
    # #include "signalsmith-linear/stft.h" resolves either via that folder or via the repo's include/ directory.
    FetchContent_Declare(signalsmith_linear
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
        SOURCE_DIR     ${CMAKE_BINARY_DIR}/ss_deps/signalsmith-linear
        SOURCE_SUBDIR  cutbench-header-only)
    FetchContent_Declare(signalsmith_stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
        SOURCE_DIR     ${CMAKE_BINARY_DIR}/ss_deps/signalsmith-stretch
        SOURCE_SUBDIR  cutbench-header-only)
    FetchContent_MakeAvailable(signalsmith_linear signalsmith_stretch)

    target_include_directories(Cutbench PRIVATE
        ${CMAKE_BINARY_DIR}/ss_deps
        ${signalsmith_linear_SOURCE_DIR}/include
        ${signalsmith_stretch_SOURCE_DIR})
    target_compile_definitions(Cutbench PRIVATE CUTBENCH_HAS_STRETCH=1)

    if(APPLE AND CUTBENCH_ACCELERATE)
        target_compile_definitions(Cutbench PRIVATE SIGNALSMITH_USE_ACCELERATE=1)
        target_link_libraries(Cutbench PRIVATE "-framework Accelerate")
    endif()
endif()
