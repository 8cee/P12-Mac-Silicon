cmake_minimum_required(VERSION 3.24)
project(P12VstHost VERSION 0.5.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the native host self-contained on Windows. Set this before JUCE is
# fetched so JUCE and the executable use the same MSVC runtime.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(P12_ENABLE_VST2 "Enable legacy VST2 hosting. Requires a legally obtained VST2 SDK and any required distribution rights." OFF)
set(P12_VST2_SDK_PATH "" CACHE PATH "Path to the legacy VST2 SDK root (only used when P12_ENABLE_VST2=ON)")

include(FetchContent)
FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

if(P12_ENABLE_VST2)
  if(NOT P12_VST2_SDK_PATH)
    message(FATAL_ERROR "P12_ENABLE_VST2=ON requires -DP12_VST2_SDK_PATH=<path-to-vst2-sdk>")
  endif()
  get_filename_component(P12_VST2_SDK_PATH "${P12_VST2_SDK_PATH}" ABSOLUTE)
  if(NOT EXISTS "${P12_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
    message(FATAL_ERROR
      "Invalid P12_VST2_SDK_PATH: ${P12_VST2_SDK_PATH}\n"
      "Expected: <sdk-root>/pluginterfaces/vst2.x/aeffect.h")
  endif()
  message(STATUS "P-12 legacy VST2 hosting: ENABLED")
  message(STATUS "P-12 VST2 SDK root: ${P12_VST2_SDK_PATH}")
  juce_set_vst2_sdk_path("${P12_VST2_SDK_PATH}")
else()
  message(STATUS "P-12 legacy VST2 hosting: disabled")
endif()

# macOS requires an app bundle for editor windows and stable bundle identity.
if(APPLE)
  juce_add_gui_app(p12-vst-host
    PRODUCT_NAME "P-12 VST Host" COMPANY_NAME "P-12"
    BUNDLE_ID "com.p12.sampler.silicon.vsthost"
    VERSION "${PROJECT_VERSION}")
else()
  juce_add_console_app(p12-vst-host
    PRODUCT_NAME "P-12 VST Host" COMPANY_NAME "P-12" VERSION "${PROJECT_VERSION}")
endif()

juce_generate_juce_header(p12-vst-host)

target_sources(p12-vst-host PRIVATE
  src/main.cpp
)

target_compile_definitions(p12-vst-host PRIVATE
  JUCE_ASIO=$<BOOL:${WIN32}>
  JUCE_WASAPI=$<BOOL:${WIN32}>
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_VST=$<BOOL:${P12_ENABLE_VST2}>
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0
  P12_ENABLE_VST2=$<BOOL:${P12_ENABLE_VST2}>
)

target_link_libraries(p12-vst-host PRIVATE
  juce::juce_audio_utils
  juce::juce_gui_extra
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags
)

if(MSVC)
  target_compile_options(p12-vst-host PRIVATE /permissive- /Zc:__cplusplus)
endif()

# Put the binary at a predictable path after a Release build.
set_target_properties(p12-vst-host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
)

# Self-contained mixer tests; no plugin or ASIO device required.
add_executable(p12-native-tests tests-native.cpp)
add_executable(p12-scheduled-tests tests-scheduled.cpp)
add_executable(p12-sp-character-tests tests-sp-character.cpp)
target_compile_features(p12-scheduled-tests PRIVATE cxx_std_20)
set_target_properties(p12-scheduled-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
target_compile_features(p12-native-tests PRIVATE cxx_std_20)
set_target_properties(p12-native-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
target_compile_features(p12-sp-character-tests PRIVATE cxx_std_20)
set_target_properties(p12-sp-character-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

# Pad control audio checks are required in Release too: keep assert() active.
add_executable(p12-pad-controls-tests tests-pad-controls.cpp)
target_compile_features(p12-pad-controls-tests PRIVATE cxx_std_20)
set_target_properties(p12-pad-controls-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
  target_compile_options(p12-pad-controls-tests PRIVATE /UNDEBUG)
else()
  target_compile_options(p12-pad-controls-tests PRIVATE -UNDEBUG)
endif()

add_executable(p12-insert-tests tests-inserts.cpp)
target_compile_features(p12-insert-tests PRIVATE cxx_std_20)
set_target_properties(p12-insert-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
  target_compile_options(p12-insert-tests PRIVATE /UNDEBUG)
else()
  target_compile_options(p12-insert-tests PRIVATE -UNDEBUG)
endif()

add_executable(p12-bus-spectrum-tests tests-bus-spectrum.cpp)
target_compile_features(p12-bus-spectrum-tests PRIVATE cxx_std_20)
set_target_properties(p12-bus-spectrum-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
 target_compile_options(p12-bus-spectrum-tests PRIVATE /UNDEBUG)
else()
 target_compile_options(p12-bus-spectrum-tests PRIVATE -UNDEBUG)
endif()

add_executable(p12-live-resources-tests tests-live-resources.cpp)
target_compile_features(p12-live-resources-tests PRIVATE cxx_std_20)
set_target_properties(p12-live-resources-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
 target_compile_options(p12-live-resources-tests PRIVATE /UNDEBUG)
else()
 target_compile_options(p12-live-resources-tests PRIVATE -UNDEBUG)
endif()

enable_testing()
foreach(test native scheduled sp-character pad-controls insert bus-spectrum live-resources)
  add_test(NAME ${test} COMMAND p12-${test}-tests)
  if(MSVC)
    target_compile_options(p12-${test}-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(p12-${test}-tests PRIVATE -UNDEBUG)
  endif()
endforeach()

option(P12_BUILD_VST3_FIXTURE "Build the local VST3 integration test plugin" OFF)
if(P12_BUILD_VST3_FIXTURE)
  juce_add_plugin(p12-test-gain
    COMPANY_NAME "P-12" PLUGIN_MANUFACTURER_CODE Ptwl PLUGIN_CODE Ptst
    FORMATS VST3 PRODUCT_NAME "P12 Test Gain" COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(p12-test-gain PRIVATE tests-gain-plugin.cpp)
  target_compile_definitions(p12-test-gain PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(p12-test-gain PRIVATE juce::juce_audio_utils)
endif()
