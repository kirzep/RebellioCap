#include "audio/mf_aac_encoder.h"
#include "audio/mf_audio_transform.h"
#include <wmcodecdsp.h>
#include <cmath>
namespace rebelliocap {
using namespace mf_audio;
struct MfAacEncoder::Impl {
 Runtime runtime; ComPtr<IMFTransform> transform; StreamDescriptor description;
 QpcTicks frequency,anchor=0,last_pts=0; std::uint64_t frames=0; std::uint32_t epoch;
 bool anchored=false,finished=false,failed=false,have_packet=false;
 Impl(QpcClock& clock,std::uint32_t e):frequency(clock.frequency()),epoch(e) {}
 void collect(std::vector<EncodedPacket>& packets,std::vector<ComPtr<IMFSample>> samples,bool draining=false) {
  for(auto& s:samples) {
   LONGLONG time=0,duration=0; check(s->GetSampleTime(&time),"AAC sample timestamp"); check(s->GetSampleDuration(&duration),"AAC sample duration");
   // A single short frame drained by the Microsoft AAC MFT has encoded
   // payload but zero sample timing. Its position is unambiguous: the first
   // 1024-sample AAC frame at the PCM anchor. Keep all other timing checks.
   if(draining && !have_packet && frames<=1024 && time==0 && duration==0) {
    time=1; duration=scale(1024,10000000,48000);
   }
   if(time<1 || duration<=0) throw Failure{E_UNEXPECTED,"invalid AAC timing"};
   const auto offset=scale(static_cast<std::uint64_t>(time-1),frequency,10000000);
   if(anchor>INT64_MAX-offset) throw Failure{E_INVALIDARG,"AAC timestamp overflow"};
   const auto pts=anchor+offset;
   if(have_packet&&pts<=last_pts) throw Failure{E_UNEXPECTED,"nonmonotonic AAC timestamp"};
   const auto end=scale(static_cast<std::uint64_t>(time-1+duration),frequency,10000000);
   auto payload=std::make_shared<const std::vector<std::byte>>(bytes(s.Get()));
   packets.push_back({description.kind,epoch,pts,pts,end-offset,true,std::move(payload)}); last_pts=pts; have_packet=true;
  }
 }
};
MfAacEncoder::MfAacEncoder(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
MfAacEncoder::~MfAacEncoder()=default;
const StreamDescriptor& MfAacEncoder::descriptor() const noexcept { return impl_->description; }
Result<std::unique_ptr<MfAacEncoder>> MfAacEncoder::create(StreamKind stream,std::uint32_t bitrate,QpcClock& clock,std::uint32_t epoch) {
 try {
  if(stream==StreamKind::Video || (bitrate!=128000&&bitrate!=160000&&bitrate!=192000) || clock.frequency()>INT32_MAX) throw Failure{E_INVALIDARG,"invalid AAC configuration"};
  auto p=std::make_unique<Impl>(clock,epoch);
  check(CoCreateInstance(CLSID_AACMFTEncoder,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&p->transform)),"create Microsoft AAC encoder");
  ComPtr<IMFMediaType> out; check(MFCreateMediaType(&out),"AAC output type");
  check(out->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"AAC major type"); check(out->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC),"AAC subtype");
  check(out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2),"AAC channels"); check(out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000),"AAC rate");
  check(out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"AAC bits"); check(out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,bitrate/8),"AAC bitrate");
  check(out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0),"raw AAC"); check(out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,0x29),"AAC-LC profile");
  check(p->transform->SetOutputType(0,out.Get(),0),"set AAC output"); auto in=pcm_type(48000,2,3,false); check(p->transform->SetInputType(0,in.Get(),0),"set AAC input");
  ComPtr<IMFMediaType> actual; check(p->transform->GetOutputCurrentType(0,&actual),"accepted AAC type");
  for(auto attribute:{MF_MT_AUDIO_NUM_CHANNELS,MF_MT_AUDIO_SAMPLES_PER_SECOND,MF_MT_AUDIO_AVG_BYTES_PER_SECOND,MF_MT_AAC_PAYLOAD_TYPE}) {
   UINT32 requested=0,accepted=0; check(out->GetUINT32(attribute,&requested),"requested attribute"); check(actual->GetUINT32(attribute,&accepted),"accepted attribute"); if(requested!=accepted) throw Failure{E_UNEXPECTED,"AAC type mismatch"};
  }
  UINT32 size=0; check(actual->GetBlobSize(MF_MT_USER_DATA,&size),"AAC user data size"); if(size<14) throw Failure{E_UNEXPECTED,"missing AAC ASC"};
  std::vector<UINT8> blob(size); check(actual->GetBlob(MF_MT_USER_DATA,blob.data(),size,&size),"AAC user data");
  // MF_MT_USER_DATA starts after WAVEFORMATEX: 12 HEAACWAVEINFO bytes, then ASC.
  if((blob[12]>>3)!=2 || (((blob[12]&7)<<1)|(blob[13]>>7))!=3 || ((blob[13]>>3)&15)!=2) throw Failure{E_UNEXPECTED,"accepted ASC is not AAC-LC 48k stereo"};
  std::vector<std::byte> asc(size-12); std::memcpy(asc.data(),blob.data()+12,asc.size());
  p->description={stream,"aac",stream_title(stream),std::move(asc),{1,static_cast<std::int32_t>(clock.frequency())},0,0,48000,2};
  start(p->transform.Get()); return Result<std::unique_ptr<MfAacEncoder>>::success(std::unique_ptr<MfAacEncoder>(new MfAacEncoder(std::move(p))));
 } catch(Failure f) { return Result<std::unique_ptr<MfAacEncoder>>::failure(error(f)); }
}
Result<std::vector<EncodedPacket>> MfAacEncoder::encode(const PcmBlock& b) {
 auto& s=*impl_; try {
  if(s.finished||s.failed) throw Failure{E_UNEXPECTED,"AAC encoder closed"};
  if(b.stream!=s.description.kind || b.sample_rate!=48000 || b.channels!=2 || (b.channel_mask&&b.channel_mask!=3) || b.interleaved.empty() || b.interleaved.size()%2 || b.interleaved.size()>UINT32_MAX/2 || b.pts<0) throw Failure{E_INVALIDARG,"expected 48k stereo PCM for encoder stream"};
  if(!s.anchored) { s.anchor=b.pts; s.anchored=true; }
  const auto expected=add_ticks(s.anchor,scale(s.frames,s.frequency,48000));
  if(std::abs(b.pts-expected)>1 || (s.frames&&b.discontinuity)) throw Failure{E_INVALIDARG,"AAC requires contiguous PCM; begin new epoch for gaps"};
  std::vector<std::int16_t> pcm; pcm.reserve(b.interleaved.size());
  for(float value:b.interleaved) {
   if(b.silent) value=0; if(!std::isfinite(value)) throw Failure{E_INVALIDARG,"nonfinite AAC input"};
   pcm.push_back(static_cast<std::int16_t>(std::lround(std::clamp(static_cast<double>(value),-1.0,1.0)*32767)));
  }
  const auto frames=b.interleaved.size()/2; const auto begin=scale(s.frames,10000000,48000),end=scale(s.frames+frames,10000000,48000);
  auto input=sample(pcm.data(),static_cast<DWORD>(pcm.size()*2),begin+1,end-begin);
  std::vector<EncodedPacket> packets; s.collect(packets,feed(s.transform.Get(),input.Get())); s.frames+=frames;
  return Result<std::vector<EncodedPacket>>::success(std::move(packets));
 } catch(Failure f) { s.failed=true; return Result<std::vector<EncodedPacket>>::failure(error(f)); }
}
Result<std::vector<EncodedPacket>> MfAacEncoder::flush() {
 auto& s=*impl_; try {
  if(s.failed) throw Failure{E_UNEXPECTED,"AAC encoder failed"}; std::vector<EncodedPacket> packets;
  if(!s.finished&&s.anchored) {
   s.collect(packets,drain(s.transform.Get()),true);
  }
  s.finished=true;
  return Result<std::vector<EncodedPacket>>::success(std::move(packets));
 } catch(Failure f) { s.failed=true; return Result<std::vector<EncodedPacket>>::failure(error(f)); }
}
}
