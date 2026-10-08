#include "audio/pcm_normalizer.h"
#include "audio/mf_audio_transform.h"
#include <wmcodecdsp.h>
#include <cmath>
#include <bit>
#include <deque>
namespace rebelliocap {
using namespace mf_audio;
struct PcmNormalizer::Impl {
 PcmNormalizationStatistics stats;
 std::unique_ptr<Runtime> runtime;
 ComPtr<IMFTransform> transform;
 QpcTicks frequency,anchor=0; std::uint64_t input_frames=0,output_frames=0,next_device=0;
 std::uint32_t rate=0,mask=0; std::uint16_t channels=0;
 bool finished=false,failed=false,device_valid=false,discontinuity=false,reconstructed=false;
 StreamKind stream=StreamKind::SystemAudio; QpcTicks raw=0;
 struct CaptureAnchor { std::uint64_t frame; QpcTicks pts; };
 std::deque<CaptureAnchor> capture_anchors;
 explicit Impl(QpcClock& c):frequency(c.frequency()) {}
 void setup(const PcmBlock& b,StreamKind target) {
  rate=b.sample_rate; channels=b.channels; mask=b.channel_mask?b.channel_mask:(channels==1?4U:3U);
  // WASAPI commonly already provides our target format. Keep the exact sample
  // data and timeline without allocating MF samples, buffers or an identity DSP.
  if(rate!=48000 || channels!=2 || mask!=3) {
   if(!runtime) runtime=std::make_unique<Runtime>();
   check(CoCreateInstance(CLSID_CResamplerMediaObject,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&transform)),"create resampler DSP");
   auto in=pcm_type(rate,channels,mask,true),out=pcm_type(48000,2,3,true);
   check(transform->SetInputType(0,in.Get(),0),"resampler input type"); check(transform->SetOutputType(0,out.Get(),0),"resampler output type"); start(transform.Get());
  }
  ++stats.segments; anchor=b.pts; input_frames=output_frames=0; stream=target; discontinuity=b.discontinuity; reconstructed=false; raw=b.raw_pts;
  capture_anchors.clear();
 }
 void capture_anchor(const PcmBlock& b) {
  const auto frame=static_cast<std::uint64_t>(scale(input_frames,48000,rate));
  // Sub-output-sample input packets can map to the same boundary. Keep one
  // anchor per boundary and prune anchors already passed by MFT output.
  while(capture_anchors.size()>1 && capture_anchors[1].frame<=output_frames) capture_anchors.pop_front();
  if(!capture_anchors.empty() && capture_anchors.back().frame==frame) capture_anchors.back().pts=b.pts;
  else capture_anchors.push_back({frame,b.pts});
  if(capture_anchors.size()>4096) throw Failure{E_UNEXPECTED,"resampler capture clock backlog exceeded"};
 }
 void collect(std::vector<PcmBlock>& out,std::vector<ComPtr<IMFSample>> samples) {
  for(auto& s:samples) {
   auto data=bytes(s.Get()); if(data.size()%8) throw Failure{E_UNEXPECTED,"partial stereo float output"};
   std::size_t copied=0;
   while(copied<data.size()/8) {
    while(capture_anchors.size()>1 && capture_anchors[1].frame<=output_frames) capture_anchors.pop_front();
    auto count=data.size()/8-copied;
    if(capture_anchors.size()>1) count=(std::min)(count,static_cast<std::size_t>(capture_anchors[1].frame-output_frames));
    PcmBlock b{stream,add_ticks(anchor,scale(output_frames,frequency,48000)),48000,2,std::vector<float>(count*2),3};
    std::memcpy(b.interleaved.data(),data.data()+copied*8,count*8);
    if(capture_anchors.empty()) throw Failure{E_UNEXPECTED,"missing resampler capture clock"};
    const auto& capture=capture_anchors.front();
    b.mixing_pts=add_ticks(capture.pts,scale(output_frames-capture.frame,frequency,48000));
    b.discontinuity=discontinuity; discontinuity=false; b.timestamp_reconstructed=reconstructed; b.raw_pts=raw;
    b.silent=std::all_of(b.interleaved.begin(),b.interleaved.end(),[](float v){return v==0;});
    stats.emitted_frames+=count; output_frames+=count; copied+=count; out.push_back(std::move(b));
   }
  }
 }
};
PcmNormalizer::PcmNormalizer(QpcClock& c):impl_(std::make_unique<Impl>(c)) {}
PcmNormalizer::~PcmNormalizer()=default;
const PcmNormalizationStatistics& PcmNormalizer::statistics() const noexcept { return impl_->stats; }
PcmNormalizer::PcmNormalizer(PcmNormalizer&&) noexcept=default;
PcmNormalizer& PcmNormalizer::operator=(PcmNormalizer&&) noexcept=default;
Result<std::vector<PcmBlock>> PcmNormalizer::normalize(PcmBlock b,StreamKind target) {
 auto& s=*impl_; try {
  if(s.finished||s.failed) throw Failure{E_UNEXPECTED,"normalizer closed"};
  if(target==StreamKind::Video || b.stream==StreamKind::Video || !b.sample_rate || b.sample_rate>192000 || !b.channels || b.channels>8 || b.interleaved.empty() || b.interleaved.size()%b.channels || b.interleaved.size()>UINT32_MAX/sizeof(float) || b.pts<0 || (b.channels>2&&!b.channel_mask) || (b.channel_mask&&std::popcount(b.channel_mask)!=b.channels)) throw Failure{E_INVALIDARG,"invalid PCM format or layout"};
  if(!b.silent && !std::all_of(b.interleaved.begin(),b.interleaved.end(),[](float v){return std::isfinite(v);})) throw Failure{E_INVALIDARG,"nonfinite PCM"};
  const auto frames=b.interleaved.size()/b.channels; if(b.device_position_valid&&b.device_position>UINT64_MAX-frames) throw Failure{E_INVALIDARG,"device position overflow"};
  std::vector<PcmBlock> result;
  bool restart=false;
  if(s.rate) {
   const auto expected=add_ticks(s.anchor,scale(s.input_frames,s.frequency,s.rate));
   s.stats.maximum_input_residual_ticks=std::max(s.stats.maximum_input_residual_ticks,std::abs(b.pts-expected));
   const bool contiguous=b.device_position_valid&&s.device_valid&&b.device_position==s.next_device;
   restart=b.discontinuity || s.rate!=b.sample_rate || s.channels!=b.channels || s.mask!=(b.channel_mask?b.channel_mask:(b.channels==1?4U:3U)) || s.stream!=target || (b.device_position_valid&&s.device_valid&&!contiguous) || (!contiguous && std::abs(b.pts-expected)>1);
  }
  if(restart && s.transform) { s.collect(result,drain(s.transform.Get())); s.transform.Reset(); }
  if(!s.rate || restart) { s.setup(b,target); s.discontinuity|=restart; }
  s.reconstructed=s.reconstructed||b.timestamp_reconstructed;
  if(!s.transform) {
   if(b.timestamp_reconstructed) ++s.stats.reconstructed_input_blocks;
   b.mixing_pts=b.pts;
   b.pts=add_ticks(s.anchor,scale(s.output_frames,s.frequency,48000));
   b.stream=target; b.channel_mask=3; b.raw_pts=s.raw;
   b.discontinuity=s.discontinuity; s.discontinuity=false;
   b.timestamp_reconstructed=s.reconstructed;
   if(b.silent) std::fill(b.interleaved.begin(),b.interleaved.end(),0.0F);
   else b.silent=std::all_of(b.interleaved.begin(),b.interleaved.end(),[](float v){return v==0;});
   s.stats.accepted_frames+=frames; s.stats.emitted_frames+=frames;
   s.input_frames+=frames; s.output_frames+=frames;
   s.device_valid=b.device_position_valid; s.next_device=b.device_position+frames;
   b.device_position=0; b.device_position_valid=false;
   result.push_back(std::move(b));
   return Result<std::vector<PcmBlock>>::success(std::move(result));
  }
  s.capture_anchor(b);
  std::vector<float> silence; const float* data=b.interleaved.data(); if(b.silent) { silence.resize(b.interleaved.size(),0); data=silence.data(); }
  const auto begin=scale(s.input_frames,10000000,s.rate),end=scale(s.input_frames+frames,10000000,s.rate);
  auto sample_in=sample(data,static_cast<DWORD>(b.interleaved.size()*sizeof(float)),begin+1,end-begin);
  s.collect(result,feed(s.transform.Get(),sample_in.Get())); s.stats.accepted_frames+=frames; if(b.timestamp_reconstructed) ++s.stats.reconstructed_input_blocks; s.input_frames+=frames; s.device_valid=b.device_position_valid; s.next_device=b.device_position+frames;
  return Result<std::vector<PcmBlock>>::success(std::move(result));
 } catch(Failure f) { s.failed=true; return Result<std::vector<PcmBlock>>::failure(error(f)); }
}
Result<std::vector<PcmBlock>> PcmNormalizer::flush() {
 auto& s=*impl_; try { if(s.failed) throw Failure{E_UNEXPECTED,"normalizer failed"}; std::vector<PcmBlock> out; if(!s.finished&&s.transform) s.collect(out,drain(s.transform.Get())); s.finished=true; return Result<std::vector<PcmBlock>>::success(std::move(out)); }
 catch(Failure f) { s.failed=true; return Result<std::vector<PcmBlock>>::failure(error(f)); }
}
}
