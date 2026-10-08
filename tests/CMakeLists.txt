# Labels keep deterministic checks separate from tests that need capture
# hardware or an interactive performance measurement.
function(rebelliocap_add_test)
  cmake_parse_arguments(ARG "" "NAME" "COMMAND;LABELS" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_COMMAND OR NOT ARG_LABELS)
    message(FATAL_ERROR "rebelliocap_add_test requires NAME, COMMAND, and LABELS.")
  endif()

  foreach(label IN LISTS ARG_LABELS)
    if(NOT label STREQUAL "unit" AND NOT label STREQUAL "hardware" AND NOT label STREQUAL "performance")
      message(FATAL_ERROR "Unsupported CTest label: ${label}")
    endif()
  endforeach()

  add_test(NAME ${ARG_NAME} COMMAND ${ARG_COMMAND})
  set_tests_properties(${ARG_NAME} PROPERTIES LABELS "${ARG_LABELS}")
endfunction()

add_custom_target(rebelliocap_tests)

find_package(Catch2 CONFIG REQUIRED)
add_executable(rebelliocap_recording_name_test unit/recording_name_test.cpp)
target_link_libraries(rebelliocap_recording_name_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME recording_name COMMAND rebelliocap_recording_name_test LABELS unit)
add_executable(rebelliocap_capture_recovery_test unit/capture_recovery_test.cpp)
target_link_libraries(rebelliocap_capture_recovery_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME capture_recovery COMMAND rebelliocap_capture_recovery_test LABELS unit)
# Include the implementation so the private peer can inject a duplication driver.
add_executable(rebelliocap_dxgi_frame_lifetime_test unit/dxgi_frame_lifetime_test.cpp)
target_link_libraries(rebelliocap_dxgi_frame_lifetime_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME dxgi_frame_lifetime COMMAND rebelliocap_dxgi_frame_lifetime_test LABELS unit)
add_executable(rebelliocap_session_controller_test unit/session_controller_test.cpp)
add_executable(rebelliocap_session_input_test unit/session_input_test.cpp)
target_link_libraries(rebelliocap_session_input_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME session_input COMMAND rebelliocap_session_input_test LABELS unit)
target_include_directories(rebelliocap_session_controller_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(rebelliocap_session_controller_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME session_controller COMMAND rebelliocap_session_controller_test LABELS unit)
rebelliocap_copy_ffmpeg_runtime(rebelliocap_session_controller_test)
add_executable(rebelliocap_continuous_recording_test unit/continuous_recording_test.cpp)
target_include_directories(rebelliocap_continuous_recording_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(rebelliocap_continuous_recording_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME continuous_recording_unit COMMAND rebelliocap_continuous_recording_test LABELS unit)
rebelliocap_copy_ffmpeg_runtime(rebelliocap_continuous_recording_test)
find_program(REBELLIOCAP_PWSH_EXECUTABLE NAMES pwsh powershell REQUIRED)
add_executable(rebelliocap_recovering_audio_source_test unit/recovering_audio_source_test.cpp)
target_link_libraries(rebelliocap_recovering_audio_source_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME recovering_audio_source COMMAND rebelliocap_recovering_audio_source_test LABELS unit)
add_executable(rebelliocap_wasapi_timeline_test unit/wasapi_timeline_test.cpp)
target_link_libraries(rebelliocap_wasapi_timeline_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME rebelliocap_wasapi_timeline_test COMMAND rebelliocap_wasapi_timeline_test LABELS unit)
add_executable(rebelliocap_wasapi_pcm_test unit/wasapi_pcm_test.cpp)
target_link_libraries(rebelliocap_wasapi_pcm_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME rebelliocap_wasapi_pcm_test COMMAND rebelliocap_wasapi_pcm_test LABELS unit)

add_executable(rebelliocap_diagnostic_writer_test
  unit/diagnostic_writer_test.cpp)
target_link_libraries(rebelliocap_diagnostic_writer_test PRIVATE
  rebelliocap_core
  Catch2::Catch2WithMain)
rebelliocap_add_test(
  NAME rebelliocap_diagnostic_writer_test
  COMMAND rebelliocap_diagnostic_writer_test
  LABELS unit)

add_executable(rebelliocap_replay_ring_test
  unit/replay_ring_test.cpp)
target_link_libraries(rebelliocap_replay_ring_test PRIVATE
  rebelliocap_core
  Catch2::Catch2WithMain)
rebelliocap_add_test(
  NAME rebelliocap_replay_ring_test
  COMMAND rebelliocap_replay_ring_test
  LABELS unit)

add_executable(rebelliocap_hotkey_chord_test
  unit/hotkey_chord_test.cpp)
target_link_libraries(rebelliocap_hotkey_chord_test PRIVATE
  rebelliocap_core
  Catch2::Catch2WithMain)
rebelliocap_add_test(
  NAME rebelliocap_hotkey_chord_test
  COMMAND rebelliocap_hotkey_chord_test
  LABELS unit)

add_executable(rebelliocap_nvenc_state_machine_test
  unit/nvenc_state_machine_test.cpp)
target_link_libraries(rebelliocap_nvenc_state_machine_test PRIVATE
  rebelliocap_core
  Catch2::Catch2WithMain)
rebelliocap_add_test(
  NAME rebelliocap_nvenc_state_machine_test
  COMMAND rebelliocap_nvenc_state_machine_test
  LABELS unit)

if(REBELLIOCAP_ENABLE_HARDWARE_TESTS)
  add_executable(rebelliocap_audio_pipeline_lifetime_test hardware/audio_pipeline_lifetime_test.cpp)
  target_include_directories(rebelliocap_audio_pipeline_lifetime_test SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries(rebelliocap_audio_pipeline_lifetime_test PRIVATE rebelliocap_core)
  rebelliocap_copy_ffmpeg_runtime(rebelliocap_audio_pipeline_lifetime_test)
  rebelliocap_add_test(NAME audio_pipeline_lifetime COMMAND rebelliocap_audio_pipeline_lifetime_test LABELS hardware)
  set_tests_properties(audio_pipeline_lifetime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  add_executable(rebelliocap_continuous_recording_hardware_test
    hardware/continuous_recording_test.cpp)
  target_link_libraries(rebelliocap_continuous_recording_hardware_test PRIVATE
    rebelliocap_core)
  rebelliocap_add_test(
    NAME continuous_recording_hardware
    COMMAND rebelliocap_continuous_recording_hardware_test
    LABELS hardware)
  set_tests_properties(continuous_recording_hardware PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 180)
  rebelliocap_copy_ffmpeg_runtime(rebelliocap_continuous_recording_hardware_test)

  add_executable(rebelliocap_audio_capture_test hardware/audio_capture_test.cpp)
  target_link_libraries(rebelliocap_audio_capture_test PRIVATE rebelliocap_core)
  rebelliocap_add_test(NAME audio_capture COMMAND rebelliocap_audio_capture_test LABELS "hardware;performance")
  set_tests_properties(audio_capture PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 330)
  add_executable(rebelliocap_audio_endpoint_catalog_hardware_test
    hardware/audio_endpoint_catalog_test.cpp)
  target_link_libraries(rebelliocap_audio_endpoint_catalog_hardware_test PRIVATE
    rebelliocap_core)
  rebelliocap_add_test(
    NAME audio_endpoint_catalog_hardware
    COMMAND rebelliocap_audio_endpoint_catalog_hardware_test
    LABELS hardware)
  set_tests_properties(audio_endpoint_catalog_hardware PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30)
  add_executable(rebelliocap_nvenc_encode_test
    hardware/nvenc_encode_test.cpp)
  target_link_libraries(rebelliocap_nvenc_encode_test PRIVATE
    rebelliocap_core
    psapi)
  rebelliocap_add_test(
    NAME nvenc_encode
    COMMAND rebelliocap_nvenc_encode_test
    LABELS "hardware;performance")
  set_tests_properties(nvenc_encode PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 720
    ENVIRONMENT "REBELLIOCAP_NVENC_SOAK_SECONDS=600")

  add_executable(rebelliocap_nv12_converter_test
    hardware/nv12_converter_test.cpp)
  target_link_libraries(rebelliocap_nv12_converter_test PRIVATE
    rebelliocap_core)
  rebelliocap_add_test(
    NAME nv12_converter
    COMMAND rebelliocap_nv12_converter_test
    LABELS hardware)
  set_tests_properties(nv12_converter PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

  add_executable(rebelliocap_dxgi_capture_test
    hardware/dxgi_capture_test.cpp)
  target_link_libraries(rebelliocap_dxgi_capture_test PRIVATE
    rebelliocap_core)
  rebelliocap_add_test(
    NAME dxgi_capture
    COMMAND rebelliocap_dxgi_capture_test
    LABELS hardware)
  set_tests_properties(dxgi_capture PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)

  add_executable(rebelliocap_hotkey_passthrough_test
    hardware/hotkey_passthrough_test.cpp)
  target_link_libraries(rebelliocap_hotkey_passthrough_test PRIVATE
    rebelliocap_core)
  rebelliocap_add_test(
    NAME hotkey_passthrough
    COMMAND rebelliocap_hotkey_passthrough_test
    LABELS hardware)
  set_tests_properties(hotkey_passthrough PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The implementation is compiled into this test TU so the private friend peer
# can inject driver functions without exporting any production fault controls.
add_executable(rebelliocap_nvenc_shutdown_test unit/nvenc_shutdown_test.cpp)
target_link_libraries(rebelliocap_nvenc_shutdown_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME rebelliocap_nvenc_shutdown_test COMMAND rebelliocap_nvenc_shutdown_test LABELS unit)
add_executable(rebelliocap_audio_mixer_test unit/audio_mixer_test.cpp)
target_link_libraries(rebelliocap_audio_mixer_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME rebelliocap_audio_mixer_test COMMAND rebelliocap_audio_mixer_test LABELS unit)
add_executable(rebelliocap_audio_endpoint_catalog_test unit/audio_endpoint_catalog_test.cpp)
target_link_libraries(rebelliocap_audio_endpoint_catalog_test PRIVATE
  rebelliocap_core
  Catch2::Catch2WithMain)
rebelliocap_add_test(
  NAME audio_endpoint_catalog_unit
  COMMAND rebelliocap_audio_endpoint_catalog_test
  LABELS unit)
if(REBELLIOCAP_ENABLE_HARDWARE_TESTS)
  add_executable(rebelliocap_aac_encoder_test hardware/aac_encoder_test.cpp)
  target_link_libraries(rebelliocap_aac_encoder_test PRIVATE rebelliocap_core)
  rebelliocap_add_test(NAME aac_encoder COMMAND rebelliocap_aac_encoder_test LABELS hardware)
  set_tests_properties(aac_encoder PROPERTIES TIMEOUT 90)
endif()
if(REBELLIOCAP_ENABLE_HARDWARE_TESTS)
  add_executable(rebelliocap_mux_output_test hardware/mux_output_test.cpp)
  target_link_libraries(rebelliocap_mux_output_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
  rebelliocap_add_test(NAME mux_output COMMAND rebelliocap_mux_output_test LABELS hardware)
  set_tests_properties(mux_output PROPERTIES TIMEOUT 90)
endif()
if(REBELLIOCAP_ENABLE_HARDWARE_TESTS)
  rebelliocap_copy_ffmpeg_runtime(rebelliocap_mux_output_test)
  target_compile_definitions(rebelliocap_mux_output_test PRIVATE REBELLIOCAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

if(REBELLIOCAP_ENABLE_HARDWARE_TESTS)
  add_executable(rebelliocap_pipeline_smoke_test hardware/pipeline_smoke_test.cpp)
  target_link_libraries(rebelliocap_pipeline_smoke_test PRIVATE rebelliocap_core)
  add_dependencies(rebelliocap_pipeline_smoke_test RebellioCap.Engine)
  rebelliocap_add_test(NAME pipeline_smoke COMMAND rebelliocap_pipeline_smoke_test LABELS hardware)
  set_tests_properties(pipeline_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  rebelliocap_copy_ffmpeg_runtime(rebelliocap_pipeline_smoke_test)
  rebelliocap_add_test(
    NAME performance_preflight
    COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
            ${PROJECT_SOURCE_DIR}/tests/scripts/performance_preflight_test.ps1
    LABELS hardware)
endif()

add_executable(rebelliocap_engine_scheduler_test unit/engine_scheduler_test.cpp)
add_executable(rebelliocap_game_category_test unit/game_category_test.cpp)
target_link_libraries(rebelliocap_game_category_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME game_category COMMAND rebelliocap_game_category_test LABELS unit)
target_include_directories(rebelliocap_engine_scheduler_test PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(rebelliocap_engine_scheduler_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME rebelliocap_engine_scheduler_test COMMAND rebelliocap_engine_scheduler_test LABELS unit)

add_executable(rebelliocap_arguments_test unit/arguments_test.cpp)
target_link_libraries(rebelliocap_arguments_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME arguments COMMAND rebelliocap_arguments_test LABELS unit)

add_executable(rebelliocap_session_protocol_test unit/session_protocol_test.cpp)
target_link_libraries(rebelliocap_session_protocol_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME session_protocol COMMAND rebelliocap_session_protocol_test LABELS unit)

add_executable(rebelliocap_pcm_windowizer_test unit/pcm_windowizer_test.cpp)
target_link_libraries(rebelliocap_pcm_windowizer_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME pcm_windowizer COMMAND rebelliocap_pcm_windowizer_test LABELS unit)

add_executable(rebelliocap_engine_metrics_json_test unit/engine_metrics_json_test.cpp)
target_link_libraries(rebelliocap_engine_metrics_json_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME engine_metrics_json COMMAND rebelliocap_engine_metrics_json_test LABELS unit)

rebelliocap_add_test(
  NAME collect_performance_analysis
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/collect_performance_test.ps1
  LABELS unit)
rebelliocap_add_test(
  NAME performance_run_order
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/performance_run_order_test.ps1
  LABELS unit)
rebelliocap_add_test(
  NAME verify_milestone_rejects_not_run
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/verify_milestone_test.ps1
  LABELS unit)
rebelliocap_add_test(
  NAME verify_desktop_fail_fast
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/verify_desktop_fail_fast_test.ps1
  LABELS unit)
rebelliocap_add_test(
  NAME rust_environment
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/rust_environment_test.ps1
  LABELS unit)
rebelliocap_add_test(
  NAME runtime_copy
  COMMAND ${REBELLIOCAP_PWSH_EXECUTABLE} -NoProfile -NonInteractive -File
          ${PROJECT_SOURCE_DIR}/tests/scripts/runtime_copy_test.ps1
  LABELS unit)

add_executable(rebelliocap_packet_budget_policy_test unit/packet_budget_policy_test.cpp)
target_link_libraries(rebelliocap_packet_budget_policy_test PRIVATE rebelliocap_core Catch2::Catch2WithMain)
rebelliocap_add_test(NAME packet_budget_policy COMMAND rebelliocap_packet_budget_policy_test LABELS unit)
