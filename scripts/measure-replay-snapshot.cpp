// Standalone developer microbenchmark, not an acceptance test. Build from repo:
// ./scripts/invoke-dev.ps1 -Command cl /nologo /std:c++20 /EHsc /O2 /DNDEBUG
//   /Isrc scripts/measure-replay-snapshot.cpp src/replay/replay_ring.cpp
//   /Febuild/replay-snapshot-bench.exe /Fobuild/
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include "replay/replay_ring.h"
using namespace rebelliocap;
using Clock = std::chrono::steady_clock;
static double micros(Clock::time_point t) { return std::chrono::duration<double,std::micro>(Clock::now()-t).count(); }
static double percentile(std::vector<double> v, double p) { std::sort(v.begin(),v.end()); return v[static_cast<size_t>((v.size()-1)*p)]; }
int main() {
  for (int seconds : {30,120,300,1800}) {
    auto budget=std::make_shared<PacketMemoryBudget>(1024ull*1024*1024);
    ReplayRing ring(static_cast<QpcTicks>(seconds)*48000,budget);
    // Metadata-only, shared tiny compressed payload: measures scan/copy/ownership,
    // not codec, realistic bitrates, mux, scheduling, or allocation pressure.
    auto payload=std::make_shared<const std::vector<std::byte>>(128);
    QpcTicks audio=0;
    const QpcTicks end=static_cast<QpcTicks>(seconds)*48000;
    for (QpcTicks video=0;video<=end;video+=800) {
      if (!ring.append({StreamKind::Video,1,video,video,800,video%96000==0,payload}).is_success()) return 2;
      while(audio<=video) {
        for(auto stream : {StreamKind::MixedAudio,StreamKind::SystemAudio})
          if(!ring.append({stream,1,audio,audio,1024,false,payload}).is_success()) return 3;
        audio+=1024;
      }
    }
    for(int clip : {30,seconds}) {
      if(seconds==30 && clip!=seconds) continue;
      std::vector<double> samples;
      size_t packets=0;
      for(int n=0;n<100;++n) {
        const auto start=Clock::now();
        auto result=ring.snapshot(end,static_cast<QpcTicks>(clip)*48000);
        samples.push_back(micros(start));
        if(!result.is_success()) return 4;
        packets=result.value().packets.size();
      }
      std::vector<ReplaySnapshot> held;
      const auto burst_start=Clock::now();
      for(int n=0;n<16;++n) {
        auto result=ring.snapshot(end,static_cast<QpcTicks>(clip)*48000);
        if(!result.is_success()) return 5;
        held.push_back(std::move(result).value());
      }
      std::printf("ring_s=%d clip_s=%d packets=%zu snapshot_p50_us=%.3f snapshot_p99_us=%.3f snapshot_max_us=%.3f burst16_us=%.3f charged_bytes=%zu\n",seconds,clip,packets,percentile(samples,.5),percentile(samples,.99),*std::max_element(samples.begin(),samples.end()),micros(burst_start),budget->bytes());
      if(seconds==30) break;
    }
    std::vector<double> appends;
    for(int n=1;n<=2400;++n) {
      const auto pts=end+static_cast<QpcTicks>(n)*800;
      const auto start=Clock::now();
      auto result=ring.append({StreamKind::Video,1,pts,pts,800,pts%96000==0,payload});
      appends.push_back(micros(start));
      if(!result.is_success()) return 6;
    }
    std::printf("ring_s=%d append_prune_p50_us=%.3f append_prune_p99_us=%.3f append_prune_max_us=%.3f samples=%zu\n",seconds,percentile(appends,.5),percentile(appends,.99),*std::max_element(appends.begin(),appends.end()),appends.size());
  }
}
