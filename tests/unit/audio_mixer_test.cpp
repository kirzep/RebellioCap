#include <catch2/catch_test_macros.hpp>
#include "audio/audio_mixer.h"
#include "audio/pcm_normalizer.h"
#include "audio/pcm_windowizer.h"
#include <cmath>
#include <chrono>
#include <iostream>
using namespace rebelliocap;
static PcmBlock block(float value) { return {StreamKind::SystemAudio, 123, 48000, 2, std::vector<float>(960,value),3}; }
TEST_CASE("mixer preserves timeline and prevents clipping") {
 auto a=block(.9F), b=block(.9F); b.stream=StreamKind::MicrophoneAudio;
 auto r=AudioMixer{}.mix(a,b,1,1); REQUIRE(r.is_success()); REQUIRE(r.value().pts==123);
 for(float v:r.value().interleaved) REQUIRE(std::abs(v)<=1);
 REQUIRE(AudioMixer{}.mix(a,b,0,0).value().interleaved[0]==0);
 b.pts++; REQUIRE_FALSE(AudioMixer{}.mix(a,b,1,1).is_success());
}
TEST_CASE("normalizer preserves buffered mono input and jitter continuity") {
 QpcClock clock; PcmNormalizer n(clock); std::size_t total=0; QpcTicks end=0;
 for(int i=0;i<100;++i) {
  PcmBlock b{StreamKind::MicrophoneAudio,123+static_cast<QpcTicks>(i)*clock.frequency()/100,44100,1,std::vector<float>(441,.2F),4};
  // Trusted raw return is only two ticks after the reconstructed previous block.
  if(i==2) b.pts=123+clock.frequency()/100+2;
  b.device_position_valid=true; b.device_position=static_cast<std::uint64_t>(i)*441; b.timestamp_reconstructed=i==1;
  auto r=n.normalize(b,StreamKind::MicrophoneAudio); REQUIRE(r.is_success());
  for(auto& o:r.value()) { REQUIRE(o.pts>=end); total+=o.interleaved.size()/2; end=o.pts; REQUIRE(o.sample_rate==48000); REQUIRE(o.channels==2); }
 }
 auto tail=n.flush(); REQUIRE(tail.is_success()); for(auto& b:tail.value()) total+=b.interleaved.size()/2;
 REQUIRE(total>=47999); REQUIRE(total<=48001); REQUIRE(n.statistics().segments==1); REQUIRE(n.statistics().reconstructed_input_blocks==1); REQUIRE(n.statistics().maximum_input_residual_ticks>=clock.frequency()/100-2); REQUIRE(n.flush().value().empty());
}

TEST_CASE("normalizer irregular sine blocks retain all frames and expose input provenance") {
 QpcClock clock; PcmNormalizer n(clock); std::uint64_t sent=0,received=0; bool saw_buffering=false;
 auto consume=[&](const std::vector<PcmBlock>& blocks) { for(const auto& b:blocks) {
  REQUIRE(b.pts==1000+static_cast<QpcTicks>(received*static_cast<std::uint64_t>(clock.frequency())/48000));
  REQUIRE_FALSE(b.device_position_valid);
  for(std::size_t j=0;j<b.interleaved.size();j+=2) REQUIRE(std::abs(b.interleaved[j]-b.interleaved[j+1])<.00001F);
  received+=b.interleaved.size()/2;
 }};
 while(sent<88200) {
  const auto count=std::min<std::uint64_t>(88200-sent,sent%2?997:13);
  PcmBlock b{StreamKind::MicrophoneAudio,1000+static_cast<QpcTicks>(sent*static_cast<std::uint64_t>(clock.frequency())/44100),44100,1,std::vector<float>(static_cast<std::size_t>(count)),4};
  for(std::size_t i=0;i<count;++i) b.interleaved[i]=.25F*static_cast<float>(std::sin(6.283185307179586*440*static_cast<double>(sent+i)/44100));
  b.timestamp_reconstructed=sent==0;
  auto result=n.normalize(b,StreamKind::MicrophoneAudio); REQUIRE(result.is_success()); saw_buffering|=result.value().empty(); consume(result.value()); sent+=count;
 }
 auto tail=n.flush(); REQUIRE(tail.is_success()); REQUIRE_FALSE(tail.value().empty()); consume(tail.value());
 REQUIRE(saw_buffering); REQUIRE(received>=95999); REQUIRE(received<=96000);
 REQUIRE(n.statistics().accepted_frames==88200); REQUIRE(n.statistics().emitted_frames==received); REQUIRE(n.statistics().reconstructed_input_blocks==1);
 REQUIRE_FALSE(n.normalize(block(0),StreamKind::SystemAudio).is_success());
}
TEST_CASE("normalizer preserves silence timestamps and drains a source gap") {
 QpcClock clock; PcmNormalizer n(clock); auto b=block(.8F); b.silent=true;
 std::size_t frames=0; auto consume=[&](const std::vector<PcmBlock>& out) { for(const auto& o:out) { frames+=o.interleaved.size()/2; REQUIRE(o.silent); for(float v:o.interleaved) REQUIRE(v==0); }};
 auto first=n.normalize(b,StreamKind::SystemAudio); REQUIRE(first.is_success()); consume(first.value());
 b.pts+=clock.frequency(); auto second=n.normalize(b,StreamKind::SystemAudio); REQUIRE(second.is_success()); consume(second.value());
 auto tail=n.flush(); REQUIRE(tail.is_success()); consume(tail.value()); REQUIRE(frames==960); REQUIRE(n.statistics().segments==2);
}
TEST_CASE("normalizer validates multichannel layout and finite PCM") {
 QpcClock clock;
 SECTION("unknown layout") { PcmNormalizer n(clock); auto b=block(0); b.channels=6; b.channel_mask=0; REQUIRE_FALSE(n.normalize(b,StreamKind::SystemAudio).is_success()); }
 SECTION("nonfinite") { PcmNormalizer n(clock); auto b=block(0); b.interleaved[0]=NAN; REQUIRE_FALSE(n.normalize(b,StreamKind::SystemAudio).is_success()); }
}
TEST_CASE("mixer gain below knee and independent stereo alignment") {
 auto a=block(.1F),b=block(.2F); b.stream=StreamKind::MicrophoneAudio;
 a.interleaved[1]=-.1F; b.interleaved[1]=-.2F;
 auto r=AudioMixer{}.mix(a,b,2,.5F); REQUIRE(r.is_success()); REQUIRE(std::abs(r.value().interleaved[0]-.3F)<.00001F); REQUIRE(std::abs(r.value().interleaved[1]+.3F)<.00001F);
 b.interleaved.pop_back(); REQUIRE_FALSE(AudioMixer{}.mix(a,b,1,1).is_success());
}

TEST_CASE("native stereo normalization transfers sample storage without buffering or copying") {
 QpcClock clock; PcmNormalizer n(clock);
 auto input=block(.125F); input.raw_pts=120; input.device_position_valid=true;
 const auto* storage=input.interleaved.data();
 auto result=n.normalize(std::move(input),StreamKind::SystemAudio);
 REQUIRE(result.is_success()); REQUIRE(result.value().size()==1);
 const auto& out=result.value().front();
 REQUIRE(out.interleaved.data()==storage);
 REQUIRE(out.interleaved==std::vector<float>(960,.125F));
 REQUIRE(out.pts==123); REQUIRE(out.raw_pts==120); REQUIRE(out.channel_mask==3);
 REQUIRE_FALSE(out.device_position_valid);
 REQUIRE(n.statistics().accepted_frames==480); REQUIRE(n.statistics().emitted_frames==480);
 REQUIRE(n.flush().value().empty());
 REQUIRE_FALSE(n.normalize(block(0),StreamKind::SystemAudio).is_success());
}

TEST_CASE("native stereo normalization handles irregular device-contiguous blocks and segment gaps") {
 QpcClock clock; PcmNormalizer n(clock);
 const std::size_t sizes[]{13,997,480,1};
 std::uint64_t frames=0;
 for(const auto count:sizes) {
  PcmBlock input{StreamKind::SystemAudio,1000+static_cast<QpcTicks>(frames)*clock.frequency()/48000,
                 48000,2,std::vector<float>(count*2,.25F),0};
  input.device_position_valid=true; input.device_position=frames;
  input.raw_pts=input.pts; input.timestamp_reconstructed=frames==13;
  if(frames==13) input.pts+=clock.frequency()/100;
  auto result=n.normalize(std::move(input),StreamKind::SystemAudio);
  REQUIRE(result.is_success()); REQUIRE(result.value().size()==1);
  const auto& out=result.value().front();
  REQUIRE(out.pts==1000+static_cast<QpcTicks>(frames)*clock.frequency()/48000);
  REQUIRE(out.interleaved.size()==count*2);
  REQUIRE(out.raw_pts==1000); REQUIRE(out.timestamp_reconstructed==(frames>=13));
  frames+=count;
 }
 REQUIRE(n.statistics().segments==1); REQUIRE(n.statistics().emitted_frames==1491);
 auto gap=block(.5F); gap.pts=clock.frequency(); gap.discontinuity=true;
 auto result=n.normalize(gap,StreamKind::SystemAudio);
 REQUIRE(result.is_success()); REQUIRE(result.value().size()==1);
 REQUIRE(result.value().front().pts==gap.pts); REQUIRE(result.value().front().discontinuity);
 REQUIRE(n.statistics().segments==2);
}

TEST_CASE("normalizer switches between native stereo and resampled mono without losing frames") {
 QpcClock clock; PcmNormalizer n(clock);
 std::size_t emitted=0;
 const auto consume=[&](const auto& result) {
  REQUIRE(result.is_success());
  for(const auto& b:result.value()) { REQUIRE(b.channels==2); REQUIRE(b.sample_rate==48000); emitted+=b.interleaved.size()/2; }
 };
 consume(n.normalize(block(.25F),StreamKind::SystemAudio));
 PcmBlock mono{StreamKind::SystemAudio,clock.frequency(),44100,1,std::vector<float>(4410,.25F),4};
 consume(n.normalize(mono,StreamKind::SystemAudio));
 auto stereo=block(.25F); stereo.pts=clock.frequency()*2;
 consume(n.normalize(stereo,StreamKind::SystemAudio));
 consume(n.flush());
 REQUIRE(emitted>=5759); REQUIRE(emitted<=5760); REQUIRE(n.statistics().segments==3);
}

TEST_CASE("native normalization bookkeeping benchmark", "[.performance]") {
 QpcClock clock;
 for(int run=0;run<5;++run) {
  PcmNormalizer normalizer(clock);
  const auto start=std::chrono::steady_clock::now();
  for(std::uint64_t i=0;i<10000;++i) {
   auto input=block(.125F); input.pts=1000+static_cast<QpcTicks>(i)*clock.frequency()/100;
   input.device_position_valid=true; input.device_position=i*480;
   REQUIRE(normalizer.normalize(std::move(input),StreamKind::SystemAudio).is_success());
  }
  REQUIRE(normalizer.flush().is_success());
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"normalizer_native_10000_blocks_ms="<<ms<<" frames="<<normalizer.statistics().emitted_frames<<'\n';
 }
}

TEST_CASE("native normalizer preserves capture clock separately from AAC sample cadence") {
 QpcClock clock; PcmNormalizer normalizer(clock);
 for(int i=0;i<100;++i) {
  auto input=block(.25F);
  const auto nominal=1000+static_cast<QpcTicks>(i)*clock.frequency()/100;
  input.pts=nominal+static_cast<QpcTicks>(i)*clock.frequency()/200000;
  input.raw_pts=input.pts+17; // Raw provenance is not the trusted/repaired clock.
  input.device_position_valid=true; input.device_position=static_cast<std::uint64_t>(i)*480;
  const auto capture=input.pts;
  const auto* storage=input.interleaved.data();
  auto result=normalizer.normalize(std::move(input),StreamKind::SystemAudio);
  REQUIRE(result.is_success()); REQUIRE(result.value().size()==1);
  const auto& output=result.value().front();
  REQUIRE(output.pts==nominal);
  REQUIRE(output.mixing_pts==capture);
  REQUIRE(output.interleaved.data()==storage);
 }
 REQUIRE(normalizer.statistics().segments==1);
}

TEST_CASE("buffered resampling associates capture QPC with corresponding output samples") {
 QpcClock clock; PcmNormalizer normalizer(clock);
 std::uint64_t received=0;
 const auto consume=[&](const auto& blocks) {
  for(const auto& output:blocks) {
   const auto index=received/480;
   REQUIRE(output.pts==1000+static_cast<QpcTicks>(received)*clock.frequency()/48000);
   const auto expected=1000+static_cast<QpcTicks>(index)*clock.frequency()/100+
       static_cast<QpcTicks>(index)*clock.frequency()/200000+
       static_cast<QpcTicks>(received%480)*clock.frequency()/48000;
   REQUIRE(output.mixing_pts==expected);
   // An output block must not straddle an input clock correction boundary.
   REQUIRE(output.interleaved.size()/2<=480-received%480);
   received+=output.interleaved.size()/2;
  }
 };
 for(int i=0;i<100;++i) {
  PcmBlock input{StreamKind::MicrophoneAudio,1000+static_cast<QpcTicks>(i)*clock.frequency()/100+
      static_cast<QpcTicks>(i)*clock.frequency()/200000,
      44100,1,std::vector<float>(441,.25F),4};
  input.device_position_valid=true; input.device_position=static_cast<std::uint64_t>(i)*441;
  auto result=normalizer.normalize(std::move(input),StreamKind::MicrophoneAudio);
  REQUIRE(result.is_success()); consume(result.value());
 }
 auto tail=normalizer.flush(); REQUIRE(tail.is_success()); consume(tail.value());
 REQUIRE(received>=47999); REQUIRE(received<=48000);
 REQUIRE(normalizer.statistics().segments==1);
}

TEST_CASE("capture clock drift reaches mixing without changing separate source PCM") {
 QpcClock clock; PcmNormalizer system(clock),microphone(clock); SynchronizedPcmWindows windows(clock.frequency());
 std::optional<SynchronizedPcmWindowPair> final;
 for(int i=0;i<=1000;++i) {
  PcmBlock a{StreamKind::SystemAudio,1000+static_cast<QpcTicks>(i)*clock.frequency()/100,
      48000,2,std::vector<float>(960,0.F),3};
  auto b=a; b.stream=StreamKind::MicrophoneAudio;
  b.pts+=static_cast<QpcTicks>(i)*clock.frequency()/1000000; // 100 ppm: 1 ms at ten seconds.
  a.device_position_valid=b.device_position_valid=true;
  a.device_position=b.device_position=static_cast<std::uint64_t>(i)*480;
  if(i==1000) { a.interleaved[96]=.25F; b.interleaved[0]=.25F; }
  auto ar=system.normalize(std::move(a),StreamKind::SystemAudio);
  auto br=microphone.normalize(std::move(b),StreamKind::MicrophoneAudio);
  REQUIRE(ar.is_success()); REQUIRE(br.is_success());
  if(i==1000) {
   REQUIRE(br.value().front().interleaved[0]==.25F);
   REQUIRE(br.value().front().pts==ar.value().front().pts);
  }
  for(const auto& output:ar.value()) windows.push_system(output);
  for(const auto& output:br.value()) windows.push_microphone(output);
  while(auto pair=windows.pop()) final=std::move(pair);
 }
 REQUIRE(final);
 REQUIRE(final->system.pts==1000+clock.frequency()*10);
 REQUIRE(final->system.interleaved[96]==.25F);
 REQUIRE(final->microphone.interleaved[96]==.25F);
 REQUIRE(final->microphone.interleaved[0]==0.F);
}

TEST_CASE("negative capture clock drift trims elapsed samples without shifting the impulse") {
 QpcClock clock; PcmNormalizer system(clock),microphone(clock); SynchronizedPcmWindows windows(clock.frequency());
 std::uint64_t emitted=0;
 std::vector<std::uint64_t> system_impulses,microphone_impulses;
 for(int i=0;i<=1000;++i) {
  PcmBlock a{StreamKind::SystemAudio,1000+static_cast<QpcTicks>(i)*clock.frequency()/100,
      48000,2,std::vector<float>(960,0.F),3};
  auto b=a; b.stream=StreamKind::MicrophoneAudio;
  b.pts-=static_cast<QpcTicks>(i)*clock.frequency()/1000000; // -100 ppm.
  a.device_position_valid=b.device_position_valid=true;
  a.device_position=b.device_position=static_cast<std::uint64_t>(i)*480;
  if(i==999) a.interleaved[864]=.25F; // 9.999 seconds on the system clock.
  if(i==1000) b.interleaved[0]=.25F; // Same physical instant on the faster device.
  auto ar=system.normalize(std::move(a),StreamKind::SystemAudio);
  auto br=microphone.normalize(std::move(b),StreamKind::MicrophoneAudio);
  REQUIRE(ar.is_success()); REQUIRE(br.is_success());
  for(const auto& output:ar.value()) windows.push_system(output);
  for(const auto& output:br.value()) windows.push_microphone(output);
  while(auto pair=windows.pop()) {
   for(std::size_t j=0;j<960;j+=2) {
    if(pair->system.interleaved[j]!=0.F) system_impulses.push_back(emitted+j/2);
    if(pair->microphone.interleaved[j]!=0.F) microphone_impulses.push_back(emitted+j/2);
   }
   emitted+=480;
  }
 }
 REQUIRE(system_impulses==std::vector<std::uint64_t>{479952});
 REQUIRE(microphone_impulses==std::vector<std::uint64_t>{479952});
 REQUIRE(windows.dropped_frames()==0); // Clock overlap trimming is not queue overflow.
}
