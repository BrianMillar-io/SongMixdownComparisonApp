cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS the plugin runs on")
endif()

project(AnalyzerAppPlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ANALYZER_BUILD_PLUGIN "Build the plugin (needs the GUI libraries on Linux)" ON)

# JUCE: a local copy with -DJUCE_DIR=..., otherwise downloaded at configure time.
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

set(CORE_SOURCES
    Source/LiveAnalyzer.cpp
    Source/MixScore.cpp
    Source/ProfileStore.cpp)

if(ANALYZER_BUILD_PLUGIN)
    if(APPLE)
        set(FORMATS AU VST3)
    else()
        set(FORMATS VST3 Standalone)
    endif()

    juce_add_plugin(AnalyzerApp
        PRODUCT_NAME "Analyzer App"
        COMPANY_NAME "Analyzer App"
        BUNDLE_ID "com.analyzerapp.mixassistant"
        PLUGIN_MANUFACTURER_CODE Anlz
        PLUGIN_CODE Mxa1
        FORMATS ${FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Analyzer Fx
        DESCRIPTION "Compares your mix with your genre profiles while it plays"
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(AnalyzerApp PRIVATE
        ${CORE_SOURCES}
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    target_compile_definitions(AnalyzerApp PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(AnalyzerApp
        PRIVATE juce::juce_audio_utils juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Renders the plugin window for a song to a PNG, to check the layout.
    juce_add_console_app(AnalyzerScreenshot PRODUCT_NAME "AnalyzerScreenshot")
    target_sources(AnalyzerScreenshot PRIVATE ${CORE_SOURCES}
        Source/PluginProcessor.cpp Source/PluginEditor.cpp tests/Screenshot.cpp)
    target_include_directories(AnalyzerScreenshot PRIVATE Source)
    target_compile_definitions(AnalyzerScreenshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Build_Standalone=0)
    target_link_libraries(AnalyzerScreenshot PRIVATE juce::juce_audio_utils juce::juce_gui_basics
        juce::juce_recommended_config_flags)
endif()

# Feeds an audio file through the live analyzer and prints what it measured,
# so the numbers can be checked against the Python analyzer.
juce_add_console_app(AnalyzerSelfTest PRODUCT_NAME "AnalyzerSelfTest")
target_sources(AnalyzerSelfTest PRIVATE ${CORE_SOURCES} tests/SelfTest.cpp)
target_include_directories(AnalyzerSelfTest PRIVATE Source)
target_compile_definitions(AnalyzerSelfTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(AnalyzerSelfTest PRIVATE juce::juce_core juce::juce_audio_formats
    juce::juce_recommended_config_flags)
