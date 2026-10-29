#include <chrono>
#include <iostream>
#include <thread>
#include "engine/hardware_pipeline_factory.h"
#include "platform/windows/com_apartment.h"

// A legacy saved game profile must now use a real display source, including
// when its display ID is missing. The game backend remains in the tree.
int main() {
  using namespace rebelliocap;
  ComApartment apartment;
  if(!apartment.initialized()) return 1;
  EngineConfig config;
  config.capture_mode=CaptureMode::Game;
  config.game_executable=L"C:/not-running/paused-game.exe";
  config.output_directory=std::filesystem::temp_directory_path();
  config.system_audio_enabled=false; config.microphone_enabled=false;
  config.width=1280; config.height=720;
  config.nvenc_config.constant_qp=true;
  config.nvenc_config.qp_i=config.nvenc_config.qp_p=config.nvenc_config.qp_b=23;
  config.recording_priority=true;
  HardwarePipelineFactory factory;
  auto created=factory.create(config);
  if(!created.is_success()) { std::cerr<<created.error().code<<": "<<created.error().message<<'\n'; return 1; }
  auto video=created.value().video;
  const auto metrics=video->metrics();
  if(!metrics.accepted_config.constant_qp || metrics.accepted_config.qp_i!=23 || metrics.accepted_config.qp_p!=23 ||
      metrics.accepted_config.qp_b!=23 || metrics.accepted_config.target_bitrate!=0 || !metrics.recording_priority_requested) return 1;
  if(metrics.recording_priority_applied && metrics.recording_gpu_priority!=7) return 1;
  std::cout<<"CQP23 accepted; GPU priority applied="<<metrics.recording_priority_applied
           <<" level="<<metrics.recording_gpu_priority<<" HRESULT="<<metrics.recording_priority_hresult.value_or(0)<<'\n';
  if(metrics.capture_session.game_window || !metrics.capture_session.available || metrics.capture_waiting_for_game) return 1;
  QpcClock clock;
  std::size_t packets=0;
  for(int tick=0;tick<12;++tick) {
    auto frame=video->tick(clock.now(),tick==0);
    if(!frame.is_success()) { std::cerr<<frame.error().code<<'\n'; return 1; }
    packets+=frame.value().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(17));
  }
  auto tail=video->flush();
  if(!tail.is_success()) return 1;
  packets+=tail.value().size(); video->suspend();
  if(packets==0) return 1;
  if(!video->resume().is_success()) return 1;
  const auto resumed=video->metrics();
  if(!resumed.accepted_config.constant_qp || resumed.accepted_config.qp_p!=23 || !resumed.recording_priority_requested ||
      resumed.recording_priority_applied!=metrics.recording_priority_applied) return 1;
  auto resumed_tail=video->flush(); if(!resumed_tail.is_success()) return 1; video->suspend();
  std::cout<<"PASS: legacy game profile uses full-screen DXGI, packets="<<packets<<'\n';
  return 0;
}
