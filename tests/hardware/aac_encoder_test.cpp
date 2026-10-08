#include "audio/mf_aac_encoder.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <array>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
using namespace rebelliocap;
static void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
int main(int argc,char** argv) {
 try {
  QpcClock clock; const std::array kinds{StreamKind::MixedAudio,StreamKind::SystemAudio,StreamKind::MicrophoneAudio};
  for (const auto count : {1U, 480U, 1023U, 1024U, 1025U, 2000U}) {
    auto short_encoder = MfAacEncoder::create(StreamKind::SystemAudio, 160000, clock);
    require(short_encoder.is_success(), "short AAC create");
    const auto anchor = clock.now();
    PcmBlock block{StreamKind::SystemAudio, anchor, 48000, 2, std::vector<float>(count * 2), 3};
    auto packets = short_encoder.value()->encode(block);
    require(packets.is_success(), "short AAC encode");
    auto drained = short_encoder.value()->flush();
    if (!drained.is_success()) std::cerr << "frames=" << count << ": " << drained.error().message << '\n';
    require(drained.is_success(), "short AAC drain");
    packets.value().insert(packets.value().end(), drained.value().begin(), drained.value().end());
    require(!packets.value().empty(), "short AAC retained");
    QpcTicks previous = anchor - 1;
    for (const auto& packet : packets.value()) {
      require(packet.pts > previous && packet.pts >= anchor && packet.duration > 0,
              "short AAC timing");
      previous = packet.pts;
    }
    require(short_encoder.value()->flush().value().empty(), "short AAC idempotent drain");
  }
  const std::array<std::uint32_t,3> rates{192000,160000,128000};
  std::array<std::unique_ptr<MfAacEncoder>,3> encoders;
  for(std::size_t stream=0;stream<3;++stream) {
   auto created=MfAacEncoder::create(kinds[stream],rates[stream],clock,7);
   if(!created.is_success()) { std::cerr<<created.error().message<<" hr="<<created.error().hresult.value_or(0)<<'\n'; return 1; }
   encoders[stream]=std::move(created).value();
  }
  for(std::size_t stream=0;stream<3;++stream) {
   auto& encoder=encoders[stream]; const auto& d=encoder->descriptor();
   nlohmann::json evidence={{"generator","Microsoft AAC MFT; deterministic sine, no user audio"},{"bitrate",rates[stream]},{"stream",static_cast<int>(kinds[stream])},{"input_frames",1440000},{"sample_rate",48000},{"channels",2},{"epoch",7},{"qpc_frequency",clock.frequency()}};
   std::vector<unsigned> asc; for(auto v:d.codec_extradata) asc.push_back(std::to_integer<unsigned>(v)); evidence["asc"]=asc; evidence["time_base"]={1,clock.frequency()};
   std::ofstream fixture; std::filesystem::path base;
   if(argc==2) { std::filesystem::create_directories(argv[1]); base=std::filesystem::path(argv[1])/("aac-"+std::to_string(stream)); fixture.open(base.string()+".bin",std::ios::binary); require(fixture.good(),"fixture open"); }
   require(d.kind==kinds[stream]&&d.codec_extradata.size()>=2,"descriptor");
   std::size_t packets=0,bytes=0; QpcTicks last=0,first=0; const auto anchor=clock.now();
   auto consume=[&](const std::vector<EncodedPacket>& output) {
    for(const auto& p:output) {
     require(p.stream==kinds[stream]&&p.epoch==7&&p.dts==p.pts&&p.duration>0&&p.payload&&!p.payload->empty(),"packet contract");
     if(packets) { require(p.pts>last,"monotonic"); require(p.pts-last<clock.frequency()/40,"cadence upper"); require(p.pts-last>clock.frequency()/50,"cadence lower"); } else first=p.pts;
     evidence["packets"].push_back({{"pts",p.pts},{"dts",p.dts},{"duration",p.duration},{"offset",bytes},{"size",p.payload->size()}});
     if(fixture.is_open()) fixture.write(reinterpret_cast<const char*>(p.payload->data()),static_cast<std::streamsize>(p.payload->size()));
     last=p.pts; ++packets; bytes+=p.payload->size();
    }
   };
   for(std::uint64_t frame=0;frame<1440000;) {
    const auto count=std::min<std::uint64_t>(1440000-frame,(frame%3==0)?479:997);
    PcmBlock b{kinds[stream],anchor+static_cast<QpcTicks>(frame*static_cast<std::uint64_t>(clock.frequency())/48000),48000,2,std::vector<float>(static_cast<std::size_t>(count*2)),3};
    for(std::uint64_t j=0;j<count;++j) { const auto x=.25F*static_cast<float>(std::sin(6.283185307179586*(440.0+110.0*static_cast<double>(stream))*static_cast<double>(frame+j)/48000.0)); b.interleaved[static_cast<std::size_t>(j*2)]=x; b.interleaved[static_cast<std::size_t>(j*2+1)]=x; }
    auto out=encoder->encode(b); if(!out.is_success()) { std::cerr<<out.error().message<<" hr="<<out.error().hresult.value_or(0)<<'\n'; return 1; } consume(out.value()); frame+=count;
   }
   auto tail=encoder->flush(); if(!tail.is_success()) { std::cerr<<tail.error().message<<" hr="<<tail.error().hresult.value_or(0)<<'\n'; return 1; } consume(tail.value());
   require(encoder->flush().value().empty(),"idempotent flush");
   require(packets>=1406&&packets<=1409,"30-second packet count"); require(last-first>=clock.frequency()*29,"timeline span");
   PcmBlock after{kinds[stream],last,48000,2,std::vector<float>(960),3}; require(!encoder->encode(after).is_success(),"encode after drain rejected");
   evidence["flush_packets"]=tail.value().size(); evidence["packet_count"]=packets; evidence["bytes"]=bytes;
   if(fixture.is_open()) { fixture.close(); require(!fixture.fail(),"fixture write"); std::ofstream json(base.string()+".json"); json<<evidence.dump(2); require(json.good(),"evidence write"); }
   std::cout<<"stream="<<static_cast<int>(kinds[stream])<<" bitrate="<<rates[stream]<<" frames=1440000 packets="<<packets<<" bytes="<<bytes<<" flush_packets="<<tail.value().size()<<" asc_bytes="<<d.codec_extradata.size()<<'\n';
  }
  return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
