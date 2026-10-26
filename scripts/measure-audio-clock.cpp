// Read-only loopback clock probe; never writes captured PCM or changes endpoints.
// Build with invoke-dev cl /std:c++20 /EHsc /O2 /DNOMINMAX /Isrc
// scripts/measure-audio-clock.cpp src/audio/wasapi_capture_source.cpp
// src/audio/wasapi_pcm.cpp src/audio/wasapi_timeline.cpp src/core/qpc_clock.cpp
// /Febuild/audio-clock-probe.exe /Fobuild/ /link ole32.lib uuid.lib
// Run: audio-clock-probe.exe PINNED_RENDER_ENDPOINT_ID 120
#include "audio/wasapi_capture_source.h"
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <iomanip>
using namespace rebelliocap;
int wmain(int argc,wchar_t** argv) {
  if(argc!=3){std::cerr<<"Expected pinned render endpoint and seconds\n";return 2;}
  const auto seconds=std::stoi(argv[2]);
  if(seconds<10||seconds>600)return 2;
  QpcClock clock;
  auto source=WasapiCaptureSource::create({AudioEndpointRole::SystemLoopback,std::wstring(argv[1])},clock);
  if(!source.is_success()){std::cerr<<source.error().code<<": "<<source.error().message<<'\n';return 1;}
  struct Anchor{QpcTicks pts;std::uint64_t position;std::uint32_t rate;};
  std::optional<Anchor> anchor;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
  auto report=std::chrono::steady_clock::now();
  std::uint64_t blocks=0,reconstructed=0,segments=0;
  std::cout<<"qpcSeconds,nominalDeviceSeconds,qpcMinusNominalMs,clockPpm,blocks,reconstructed,segments\n"<<std::setprecision(12);
  while(std::chrono::steady_clock::now()<deadline){
    auto next=source.value()->next_block(std::chrono::milliseconds(100));
    if(!next.is_success()){std::cerr<<next.error().code<<": "<<next.error().message<<'\n';return 1;}
    if(!next.value())continue;
    const auto& b=*next.value();++blocks;reconstructed+=b.timestamp_reconstructed;
    if(!anchor||b.discontinuity||b.device_position<anchor->position||b.sample_rate!=anchor->rate){
      anchor=Anchor{b.pts,b.device_position,b.sample_rate};++segments;
    }
    if(std::chrono::steady_clock::now()>=report){
      const auto qpc=static_cast<long double>(b.pts-anchor->pts)/clock.frequency();
      const auto nominal=static_cast<long double>(b.device_position-anchor->position)/anchor->rate;
      std::cout<<qpc<<','<<nominal<<','<<(qpc-nominal)*1000<<','<<(nominal?(qpc/nominal-1)*1e6L:0)<<','<<blocks<<','<<reconstructed<<','<<segments<<std::endl;
      report=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    }
  }
  return blocks?0:1;
}
