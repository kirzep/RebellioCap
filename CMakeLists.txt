cmake_minimum_required(VERSION 3.28)

project(RebellioCap VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "RebellioCap requires the Windows x64 MSVC toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "RebellioCap supports x64 builds only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/CopyRuntimeDependencies.cmake)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

add_library(rebelliocap_core STATIC
  src/cli/arguments.cpp
  src/cli/engine_metrics_json.cpp
  src/audio/audio_endpoint_catalog.cpp
  src/audio/audio_mixer.cpp
  src/audio/pcm_windowizer.cpp
  src/audio/pcm_normalizer.cpp
  src/audio/mf_aac_encoder.cpp
  src/audio/wasapi_capture_source.cpp
  src/audio/wasapi_pcm.cpp
  src/audio/wasapi_timeline.cpp
  src/capture/d3d11_device.cpp
  src/capture/dxgi_capture_source.cpp
  src/capture/monitor_catalog.cpp
  src/core/diagnostic_writer.cpp
  src/core/qpc_clock.cpp
  src/engine/recording_name.cpp
  src/engine/recorder_engine.cpp
  src/engine/game_category.cpp
  src/engine/game_icon.cpp
  src/engine/continuous_recording.cpp
  src/engine/session_protocol.cpp
  src/engine/session_controller.cpp
  src/engine/session_input.cpp
  src/engine/hardware_pipeline_factory.cpp
  src/hotkey/hotkey_chord.cpp
  src/hotkey/raw_input_hotkey.cpp
  src/mux/ffmpeg_clip_muxer.cpp
  src/mux/ffmpeg_continuous_muxer.cpp
  src/replay/replay_ring.cpp
  src/video/d3d11_nv12_converter.cpp
  src/video/nvenc_api.cpp
  src/video/nvenc_encoder.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/contracts/recording-settings.v1.json")
file(READ "${PROJECT_SOURCE_DIR}/contracts/recording-settings.v1.json" RECORDING_CONTRACT_JSON)
configure_file(src/config/recording_contract_json.h.in generated/config/recording_contract_json.h @ONLY)
target_include_directories(rebelliocap_core PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/generated)
target_include_directories(rebelliocap_core SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nvapi)
target_compile_options(rebelliocap_core PRIVATE
  /W4
  /WX
  /permissive-
  /Zc:__cplusplus
  /utf-8
  /sdl)
target_link_libraries(rebelliocap_core PUBLIC
  nlohmann_json::nlohmann_json
  d3d11
  dxgi
  mfplat
  mf
  mfuuid
  wmcodecdspuuid
  ole32
  avrt
  user32)
rebelliocap_enable_warnings(rebelliocap_core)

add_library(rebelliocap_engine INTERFACE)
target_link_libraries(rebelliocap_engine INTERFACE
  rebelliocap_core
  d3d11
  dxgi
  mfplat
  mf
  mfuuid
  avrt)
rebelliocap_enable_warnings(rebelliocap_engine)

add_executable(RebellioCap.Engine src/cli/main.cpp src/cli/prepare_playback.cpp)
target_include_directories(RebellioCap.Engine SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_compile_options(RebellioCap.Engine PRIVATE /utf-8)
set_target_properties(RebellioCap.Engine PROPERTIES OUTPUT_NAME "RebellioCap.Engine")
target_link_libraries(RebellioCap.Engine PRIVATE rebelliocap_core)
rebelliocap_enable_warnings(RebellioCap.Engine)
rebelliocap_copy_ffmpeg_runtime(RebellioCap.Engine)

enable_testing()
add_subdirectory(tests)

# Read-only diagnostics for the local driver catalog, never packaged database data.
add_executable(rebelliocap_game_catalog_probe tools/game-catalog-probe.cpp)
target_link_libraries(rebelliocap_game_catalog_probe PRIVATE rebelliocap_core)

target_include_directories(rebelliocap_core SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
# Link only the mux/demux, packet and utility libraries; no encoder API is used.
target_link_libraries(rebelliocap_core PUBLIC ${FFMPEG_libavformat_LIBRARY} ${FFMPEG_libavcodec_LIBRARY} ${FFMPEG_libavutil_LIBRARY})
