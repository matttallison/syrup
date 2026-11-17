cmake_minimum_required(VERSION 3.22)
project(SyrupAcid VERSION 1.0.0)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
  set(ACID_FORMATS AU VST3 AUv3 Standalone)
else()
  set(ACID_FORMATS VST3)
endif()

juce_add_plugin(SyrupAcid
  COMPANY_NAME "Soul Provider Records"
  BUNDLE_ID "com.soulprovider.syrupacid"
  PLUGIN_MANUFACTURER_CODE Soul
  PLUGIN_CODE SyAc
  PRODUCT_NAME "Syrup Acid"
  FORMATS ${ACID_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT TRUE
  IS_MIDI_EFFECT TRUE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_CATEGORIES Fx Generator
  APP_SANDBOX_ENABLED TRUE
  APP_SANDBOX_OPTIONS com.apple.security.device.audio-input
  MICROPHONE_PERMISSION_ENABLED FALSE)

target_sources(SyrupAcid PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(SyrupAcid PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(SyrupAcid PRIVATE juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
