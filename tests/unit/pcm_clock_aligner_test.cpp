#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <array>
#include <algorithm>
#include <iostream>
#include "audio/pcm_clock_aligner.h"
#include "audio/pcm_epoch_bridge.h"
#include "audio/pcm_windowizer.h"
#include "audio/audio_mixer.h"
using namespace rebelliocap;
TEST_CASE("clock correction prevents capture jitter cuts in the mixed waveform") {
  constexpr QpcTicks frequency=10'000'000,anchor=100000;
  constexpr double pi=3.141592653589793,tone=1100.;
  std::array<double,2> residuals{};
  for(const bool corrected:{false,true}){
    PcmClockAligner aligner(frequency);PcmEpochBridge bridge(frequency);
    SynchronizedPcmWindows windows(frequency);AudioMixer mixer;
    std::vector<float> samples;
    const auto drain=[&](bool final){while(auto pair=windows.pop(final)){
      auto mixed=mixer.mix(pair->system,pair->microphone,1.F,1.F);
      REQUIRE(mixed.is_success());
      for(std::size_t i=0;i<mixed.value().interleaved.size();i+=2)samples.push_back(mixed.value().interleaved[i]);
    }};
    const auto consume=[&](const std::vector<PcmBlock>& blocks){for(auto block:blocks){
      auto accepted=bridge.accept(std::move(block),[&](const PcmBlock& source){
        windows.push_system(source);
        auto microphone=source;microphone.stream=StreamKind::MicrophoneAudio;
        microphone.silent=true;std::fill(microphone.interleaved.begin(),microphone.interleaved.end(),0.F);
        windows.push_microphone(microphone);drain(false);
        return Result<void>::success();
      });REQUIRE(accepted.is_success());
    }};
    for(int packet=0;packet<2000;++packet){
      PcmBlock block{StreamKind::SystemAudio,anchor+static_cast<QpcTicks>(packet)*frequency/100,48000,2,std::vector<float>(960),3};
      block.mixing_pts=block.pts+(packet%2?10000:-10000);
      for(int frame=0;frame<480;++frame){
        const auto value=static_cast<float>(.1*std::sin(2*pi*tone*(packet*480+frame)/48000));
        block.interleaved[frame*2]=block.interleaved[frame*2+1]=value;
      }
      if(corrected){auto result=aligner.push(std::move(block));REQUIRE(result.is_success());consume(result.value());}
      else consume({std::move(block)});
    }
    if(corrected){auto result=aligner.flush();REQUIRE(result.is_success());consume(result.value());}
    drain(true);REQUIRE(samples.size()>480128);
    long double energy=0,error_energy=0;
    const double coefficient=2*std::cos(2*pi*tone/48000);
    for(std::size_t i=480000;i+1024<samples.size();++i){
      const double value=samples[i],error=value-coefficient*samples[i-1]+samples[i-2];
      energy+=value*value;error_energy+=error*error;
    }
    residuals[corrected?1:0]=10*std::log10(static_cast<double>(error_energy/energy));
    std::cout<<"mixed_clock_corrected="<<corrected<<" residual_db="<<residuals[corrected?1:0]<<'\n';
  }
  // The bypass is the old placement path: the same packets acquire hard cuts.
  // Corrected blocks enter the bridge/windowizer on a contiguous sample grid.
  CHECK(residuals[0]>-40);
  CHECK(residuals[1]<-60);
  CHECK(residuals[1]<residuals[0]-30);
}
TEST_CASE("soft audio clock alignment follows both drift signs without accumulating A/V phase") {
  constexpr QpcTicks frequency=10'000'000,anchor=1000;
  for(const double ppm:{-1000.,-100.,100.,1000.}) {
    CAPTURE(ppm);
    PcmClockAligner aligner(frequency);
    std::uint64_t output_frames=0;
    const auto consume=[&](const std::vector<PcmBlock>& blocks){
      for(const auto& b:blocks){
        REQUIRE(b.pts==anchor+static_cast<QpcTicks>(static_cast<long double>(output_frames)*frequency/48000));
        REQUIRE(b.mixing_pts==b.pts);
        output_frames+=b.interleaved.size()/2;
      }
    };
    for(int i=0;i<12000;++i){
      PcmBlock b{StreamKind::SystemAudio,anchor+static_cast<QpcTicks>(i)*frequency/100,48000,2,std::vector<float>(960,0.F),3};
      b.mixing_pts=anchor+static_cast<QpcTicks>(static_cast<long double>(i)*frequency/100*(1+ppm/1e6));
      auto out=aligner.push(std::move(b));REQUIRE(out.is_success());consume(out.value());
    }
    auto tail=aligner.flush();REQUIRE(tail.is_success());consume(tail.value());
    const double target=120.*48000*(1+ppm/1e6);
    REQUIRE(std::abs(static_cast<double>(output_frames)-target)<240); // Under5ms after120s, including phase smoothing.
  }
}

TEST_CASE("zero drift retains every source frame and stable PCM through final drain") {
  constexpr QpcTicks f=10'000'000;
  PcmClockAligner aligner(f);
  std::vector<float> samples;
  const auto consume=[&](const auto& blocks){for(const auto& b:blocks)samples.insert(samples.end(),b.interleaved.begin(),b.interleaved.end());};
  for(int i=0;i<100;++i){
    PcmBlock b{StreamKind::SystemAudio,1000+static_cast<QpcTicks>(i)*f/100,48000,2,std::vector<float>(960,.25F),3};
    b.mixing_pts=b.pts;auto out=aligner.push(std::move(b));REQUIRE(out.is_success());consume(out.value());
  }
  auto tail=aligner.flush();REQUIRE(tail.is_success());consume(tail.value());
  REQUIRE(samples.size()==96000);
  for(std::size_t i=128;i+128<samples.size();++i)REQUIRE(std::abs(samples[i]-.25F)<.00001F);
  REQUIRE(aligner.flush().value().empty());
  PcmBlock after{StreamKind::SystemAudio,1000+f,48000,2,std::vector<float>(960),3};
  REQUIRE_FALSE(aligner.push(std::move(after)).is_success());
}

TEST_CASE("audio clock correction smooths capture jitter without waveform cuts") {
  constexpr QpcTicks f=10'000'000;
  PcmClockAligner aligner(f);
  std::optional<float> previous;
  double maximum_step=0;
  std::uint64_t frames=0;
  const auto consume=[&](const auto& blocks){for(const auto& b:blocks){
    for(std::size_t i=0;i<b.interleaved.size();i+=2){
      REQUIRE(std::isfinite(b.interleaved[i]));
      REQUIRE(std::abs(b.interleaved[i]-b.interleaved[i+1])<.000001F);
      if(previous)maximum_step=(std::max)(maximum_step,std::abs(static_cast<double>(b.interleaved[i]-*previous)));
      previous=b.interleaved[i];++frames;
    }
  }};
  for(int i=0;i<2000;++i){
    PcmBlock b{StreamKind::SystemAudio,100000+static_cast<QpcTicks>(i)*f/100,48000,2,std::vector<float>(960),3};
    b.mixing_pts=b.pts+(i%2?10000:-10000); // +/-1ms packet noise, no physical sample drift.
    for(int j=0;j<480;++j){const auto value=static_cast<float>(.25*std::sin(2*3.141592653589793*1000*(i*480+j)/48000));b.interleaved[j*2]=b.interleaved[j*2+1]=value;}
    auto out=aligner.push(std::move(b));REQUIRE(out.is_success());consume(out.value());
  }
  auto tail=aligner.flush();REQUIRE(tail.is_success());consume(tail.value());
  CHECK(maximum_step<.05);
  CHECK(std::abs(static_cast<double>(frames)-20*48000)<96);
  CHECK(aligner.statistics().compensations>0);
}

TEST_CASE("audio clock alignment preserves explicit recovery boundaries and silence") {
  constexpr QpcTicks f=10'000'000;
  PcmClockAligner aligner(f);
  for(int i=0;i<2;++i){
    PcmBlock b{StreamKind::SystemAudio,1000+static_cast<QpcTicks>(i)*f,48000,2,std::vector<float>(960,99.F),3};
    b.silent=true;b.discontinuity=i!=0;b.mixing_pts=b.pts;
    auto out=aligner.push(std::move(b));REQUIRE(out.is_success());
    bool boundary=false;
    for(const auto& block:out.value()){
      REQUIRE(block.silent);for(const auto v:block.interleaved)REQUIRE(v==0);
      if(block.discontinuity){CHECK(block.pts==1000+f);boundary=true;}
    }
    if(i)CHECK(boundary);
  }
  auto tail=aligner.flush();REQUIRE(tail.is_success());
  CHECK(aligner.statistics().segments==2);
  CHECK(aligner.statistics().input_frames==960);
  CHECK(aligner.statistics().output_frames==960);
}

TEST_CASE("audio clock alignment rejects invalid and unbounded PCM") {
  for(int mode=0;mode<7;++mode){
    CAPTURE(mode);
    PcmClockAligner aligner(mode==0?0:10'000'000);
    PcmBlock b{StreamKind::SystemAudio,1000,48000,2,std::vector<float>(960,.1F),3};
    if(mode==1)b.channels=1;
    if(mode==2)b.interleaved[0]=std::numeric_limits<float>::quiet_NaN();
    if(mode==3)b.interleaved.resize(96002);
    if(mode==4)b.mixing_pts=-1;
    if(mode==5)b.stream=StreamKind::Video;
    if(mode==6)b.interleaved.clear();
    REQUIRE_FALSE(aligner.push(std::move(b)).is_success());
    REQUIRE_FALSE(aligner.flush().is_success());
  }
}

TEST_CASE("audio clock alignment drains even a sub-filter-length source exactly once") {
  for(const int count:{1,2,16,17,31,32,63,64,65,479,1024}){
    CAPTURE(count);
    PcmClockAligner aligner(10'000'000);
    PcmBlock b{StreamKind::SystemAudio,1000,48000,2,std::vector<float>(static_cast<std::size_t>(count)*2,.2F),3};
    auto first=aligner.push(std::move(b));REQUIRE(first.is_success());
    auto tail=aligner.flush();REQUIRE(tail.is_success());
    std::size_t samples=0;
    for(const auto& block:first.value())samples+=block.interleaved.size()/2;
    for(const auto& block:tail.value())samples+=block.interleaved.size()/2;
    CHECK(samples==count);
    CHECK(aligner.flush().value().empty());
  }
}

TEST_CASE("audio clock alignment rejects output timestamp overflow before publication") {
  PcmClockAligner aligner(10'000'000);
  PcmBlock b{StreamKind::SystemAudio,std::numeric_limits<QpcTicks>::max()-1,48000,2,std::vector<float>(960,.2F),3};
  REQUIRE_FALSE(aligner.push(std::move(b)).is_success());
}

TEST_CASE("clock compensation survives irregular batches and an explicit recovery gap") {
  constexpr QpcTicks f=10'000'000,anchor=1000;
  for(const double ppm:{-1000.,1000.}){
    CAPTURE(ppm);
    PcmClockAligner aligner(f);PcmEpochBridge bridge(f);
    std::uint64_t emitted=0;
    const auto consume=[&](const auto& blocks){for(auto block:blocks){
      const auto accepted=bridge.accept(std::move(block),[&](const PcmBlock& b){
        CHECK(b.pts==anchor+static_cast<QpcTicks>(static_cast<long double>(emitted)*f/48000));
        CHECK_FALSE(b.discontinuity);
        emitted+=b.interleaved.size()/2;
        return Result<void>::success();
      });REQUIRE(accepted.is_success());
    }};
    constexpr std::uint64_t total=480000,boundary=240000;
    std::uint64_t input=0;std::size_t batch=0;
    const std::array sizes{1U,31U,479U,997U,64U};
    while(input<total){
      const auto remaining=input<boundary?boundary-input:total-input;
      const auto count=(std::min)(static_cast<std::uint64_t>(sizes[batch++%sizes.size()]),remaining);
      const auto capture=anchor+static_cast<QpcTicks>(static_cast<long double>(input)*f/48000*(1+ppm/1e6))+(input>=boundary?f/10:0);
      PcmBlock b{StreamKind::SystemAudio,capture,48000,2,std::vector<float>(static_cast<std::size_t>(count)*2,.2F),3};
      b.mixing_pts=capture;b.discontinuity=input==boundary;
      auto out=aligner.push(std::move(b));REQUIRE(out.is_success());consume(out.value());input+=count;
    }
    auto tail=aligner.flush();REQUIRE(tail.is_success());consume(tail.value());
    CHECK(aligner.statistics().segments==2);
    CHECK(std::abs(static_cast<double>(emitted)-(10*(1+ppm/1e6)+.1)*48000)<240);
  }
}

TEST_CASE("soft clock correction preserves steady tones without broadband discontinuities") {
  constexpr QpcTicks f=10'000'000,anchor=100000;
  constexpr double pi=3.141592653589793;
  for(const double ppm:{-1000.,1000.})for(const double tone:{1000.,6000.,12000.}){
    CAPTURE(ppm,tone);
    PcmClockAligner aligner(f);std::vector<float> samples;
    samples.reserve(1'000'000);
    const auto consume=[&](const auto& blocks){for(const auto& b:blocks)for(std::size_t i=0;i<b.interleaved.size();i+=2)samples.push_back(b.interleaved[i]);};
    for(int i=0;i<2000;++i){
      PcmBlock b{StreamKind::SystemAudio,anchor+static_cast<QpcTicks>(i)*f/100,48000,2,std::vector<float>(960),3};
      b.mixing_pts=anchor+static_cast<QpcTicks>(static_cast<long double>(i)*f/100*(1+ppm/1e6))+(i%2?10000:-10000);
      for(int j=0;j<480;++j){const auto value=static_cast<float>(.25*std::sin(2*pi*tone*(i*480+j)/48000));b.interleaved[j*2]=b.interleaved[j*2+1]=value;}
      auto out=aligner.push(std::move(b));REQUIRE(out.is_success());consume(out.value());
    }
    auto tail=aligner.flush();REQUIRE(tail.is_success());consume(tail.value());
    REQUIRE(samples.size()>480128);
    const double coefficient=2*std::cos(2*pi*tone/(1+ppm/1e6)/48000);
    long double energy=0,residual=0;std::size_t count=0;
    // Ignore controller warmup and the finite-segment filter edges. A sinusoid
    // obeys this recurrence regardless of constant phase; hard sample cuts do
    // not. This is a broadband continuity check, not a perceptual music score.
    for(std::size_t i=480000;i+128<samples.size();++i){
      const double value=samples[i],error=value-coefficient*samples[i-1]+samples[i-2];
      energy+=value*value;residual+=error*error;++count;
    }
    const double residual_db=10*std::log10(static_cast<double>(residual/energy));
    const double gain=std::sqrt(static_cast<double>(energy)/static_cast<double>(count))/(.25/std::sqrt(2.));
    INFO("oscillator residual dB="<<residual_db<<" gain="<<gain);
    CHECK(residual_db < -60);
    CHECK(std::abs(gain-1.) < .01);
    std::cout<<"audio_clock_tone="<<tone<<" ppm="<<ppm<<" residual_db="<<residual_db<<" gain="<<gain<<'\n';
  }
}
