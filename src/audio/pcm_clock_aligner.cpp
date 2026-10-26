#include "audio/pcm_clock_aligner.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace rebelliocap {
namespace {
constexpr int rate=48000,maximum_input=48000,maximum_output=96000;
void checked(int result,const char* action){if(result<0)throw std::runtime_error(action);}
Error failure(const std::exception& e){return {"audio.clock_alignment_failed",e.what(),{}};}
}
struct PcmClockAligner::Impl {
  explicit Impl(QpcTicks f):frequency(f){}
  ~Impl(){swr_free(&resampler);}
  PcmClockAlignmentStatistics stats;
  QpcTicks frequency,anchor=0,raw=0;
  SwrContext* resampler=nullptr;
  std::uint64_t input=0,output=0,next_compensation=rate;
  double smoothed_phase=0;
  StreamKind stream=StreamKind::SystemAudio;
  bool finished=false,failed=false,discontinuity=false,reconstructed=false;
  std::vector<float> startup;
  QpcTicks timestamp(std::uint64_t frames)const{
    const auto limit=static_cast<std::uint64_t>(std::numeric_limits<QpcTicks>::max());
    const auto f=static_cast<std::uint64_t>(frequency),whole=frames/rate,remainder=frames%rate;
    if(whole>limit/f)throw std::runtime_error("Audio clock timestamp overflow");
    const auto fraction=remainder*(f/rate)+remainder*(f%rate)/rate;
    if(fraction>limit-whole*f)throw std::runtime_error("Audio clock timestamp overflow");
    const auto offset=whole*f+fraction;
    if(static_cast<std::uint64_t>(anchor)>limit-offset)throw std::runtime_error("Audio clock timestamp overflow");
    return anchor+static_cast<QpcTicks>(offset);
  }
  void setup(const PcmBlock& b){
    swr_free(&resampler);
    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
    checked(swr_alloc_set_opts2(&resampler,&stereo,AV_SAMPLE_FMT_FLT,rate,&stereo,AV_SAMPLE_FMT_FLT,rate,0,nullptr),"Allocate audio clock resampler");
    checked(av_opt_set_int(resampler,"flags",SWR_FLAG_RESAMPLE,0),"Enable soft audio resampling");
    checked(swr_init(resampler),"Initialize audio clock resampler");
    anchor=b.mixing_pts.value_or(b.pts);raw=b.raw_pts;stream=b.stream;
    input=output=0;next_compensation=rate;smoothed_phase=0;
    startup.clear();
    discontinuity=b.discontinuity;reconstructed=b.timestamp_reconstructed;++stats.segments;
  }
  void publish(std::vector<float> pcm,std::vector<PcmBlock>& blocks){
    const auto emitted=pcm.size()/2;
    if(emitted>UINT64_MAX-output)throw std::runtime_error("Audio clock frame count overflow");
    const auto pts=timestamp(output);
    static_cast<void>(timestamp(output+emitted));
    PcmBlock b{stream,pts,rate,2,std::move(pcm),3};
    b.mixing_pts=b.pts;b.raw_pts=raw;b.discontinuity=discontinuity;
    b.timestamp_reconstructed=reconstructed;
    b.silent=std::all_of(b.interleaved.begin(),b.interleaved.end(),[](float v){return v==0;});
    discontinuity=false;output+=emitted;stats.output_frames+=emitted;
    blocks.push_back(std::move(b));
  }
  int convert(const float* data,int count,std::vector<PcmBlock>& blocks){
    const int capacity=swr_get_out_samples(resampler,count);
    if(capacity<0||capacity>maximum_output)throw std::runtime_error("Audio clock output bound exceeded");
    std::vector<float> pcm(static_cast<std::size_t>(capacity)*2);
    auto* destination=reinterpret_cast<std::uint8_t*>(pcm.data());
    const auto* source=reinterpret_cast<const std::uint8_t*>(data);
    const int emitted=swr_convert(resampler,&destination,capacity,data?&source:nullptr,count);
    checked(emitted,"Convert audio clock samples");
    if(!emitted)return 0;
    pcm.resize(static_cast<std::size_t>(emitted)*2);
    publish(std::move(pcm),blocks);return emitted;
  }
  void drain(std::vector<PcmBlock>& blocks){
    // libswresample cannot retain every sample when an entire segment is shorter
    // than its initial filter support. Keep at most63 startup frames untouched;
    // their sub1.4ms duration does not require a clock-rate estimate.
    if(!startup.empty()){publish(std::move(startup),blocks);startup.clear();return;}
    for(int i=0;i<8;++i)if(!convert(nullptr,0,blocks))return;
    throw std::runtime_error("Audio clock resampler did not drain within bound");
  }
};
PcmClockAligner::PcmClockAligner(QpcTicks f):impl_(std::make_unique<Impl>(f)){}
PcmClockAligner::~PcmClockAligner()=default;
Result<std::vector<PcmBlock>> PcmClockAligner::push(PcmBlock b){
  auto& s=*impl_;
  try {
    if(s.finished||s.failed)throw std::runtime_error("Audio clock aligner is closed");
    if(s.frequency<=0||b.stream==StreamKind::Video||b.sample_rate!=rate||b.channels!=2||
       (b.channel_mask&&b.channel_mask!=3)||b.interleaved.empty()||b.interleaved.size()%2||
       b.interleaved.size()/2>maximum_input||b.pts<0||b.mixing_pts.value_or(b.pts)<0)
      throw std::runtime_error("Expected bounded normalized48k stereo PCM");
    if(b.silent)std::fill(b.interleaved.begin(),b.interleaved.end(),0.F);
    else if(!std::all_of(b.interleaved.begin(),b.interleaved.end(),[](float v){return std::isfinite(v);}))
      throw std::runtime_error("Nonfinite audio clock input");
    std::vector<PcmBlock> blocks;
    if(s.resampler&&(b.discontinuity||b.stream!=s.stream)){
      s.drain(blocks);swr_free(&s.resampler);
    }
    if(!s.resampler)s.setup(b);
    const int count=static_cast<int>(b.interleaved.size()/2);
    const auto capture=b.mixing_pts.value_or(b.pts);
    const double phase=static_cast<double>((static_cast<long double>(capture)-s.anchor)*rate/s.frequency-s.input);
    // Smooth packet jitter; correct samples gradually, never encoded timestamps.
    const double alpha=static_cast<double>(count)/(2.*rate+count);
    s.smoothed_phase+=alpha*(phase-s.smoothed_phase);
    if(s.input>=s.next_compensation){
      const auto delay=swr_get_delay(s.resampler,rate);
      if(delay<0||delay>maximum_input)throw std::runtime_error("Audio clock delay bound exceeded");
      const double error=static_cast<double>(s.input)+s.smoothed_phase-static_cast<double>(s.output)-static_cast<double>(delay);
      s.stats.maximum_phase_error_frames=(std::max)(s.stats.maximum_phase_error_frames,std::abs(error));
      if(!std::isfinite(error)||std::abs(error)>rate/2)throw std::runtime_error("Audio capture clock requires a new segment");
      const int delta=static_cast<int>(std::llround(std::clamp(error,-96.,96.)));
      checked(swr_set_compensation(s.resampler,delta,rate),"Set soft audio clock compensation");
      ++s.stats.compensations;s.next_compensation=s.input+rate;
    }
    s.reconstructed|=b.timestamp_reconstructed;
    if(!s.startup.empty()||(s.input==0&&count<64)){
      s.startup.insert(s.startup.end(),b.interleaved.begin(),b.interleaved.end());
      if(s.startup.size()/2>=64){
        s.convert(s.startup.data(),static_cast<int>(s.startup.size()/2),blocks);
        s.startup.clear();
      }
    }else s.convert(b.interleaved.data(),count,blocks);
    s.input+=count;s.stats.input_frames+=count;
    return Result<std::vector<PcmBlock>>::success(std::move(blocks));
  } catch(const std::exception& e){s.failed=true;return Result<std::vector<PcmBlock>>::failure(failure(e));}
}
Result<std::vector<PcmBlock>> PcmClockAligner::flush(){
  auto& s=*impl_;
  try{
    if(s.failed)throw std::runtime_error("Audio clock aligner failed");
    std::vector<PcmBlock> blocks;
    if(!s.finished&&s.resampler)s.drain(blocks);
    s.finished=true;return Result<std::vector<PcmBlock>>::success(std::move(blocks));
  }catch(const std::exception& e){s.failed=true;return Result<std::vector<PcmBlock>>::failure(failure(e));}
}
const PcmClockAlignmentStatistics& PcmClockAligner::statistics()const noexcept{return impl_->stats;}
}
