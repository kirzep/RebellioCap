#include "audio/audio_mixer.h"
#include <cmath>
namespace rebelliocap {
Result<PcmBlock> AudioMixer::mix(const PcmBlock& a,const PcmBlock& b,float ga,float gb) {
 if(a.sample_rate!=48000 || b.sample_rate!=48000 || a.channels!=2 || b.channels!=2 || (a.channel_mask&&a.channel_mask!=3) || (b.channel_mask&&b.channel_mask!=3) || a.pts!=b.pts || a.interleaved.size()!=960 || b.interleaved.size()!=960 || !std::isfinite(ga)|| !std::isfinite(gb))
  return Result<PcmBlock>::failure({"audio_mix_alignment","Expected aligned 10ms float stereo windows",{}});
 PcmBlock o{StreamKind::MixedAudio,a.pts,48000,2,{},3}; o.interleaved.reserve(960);
 for(std::size_t i=0;i<960;++i) {
  const double x=(a.silent?0.0:a.interleaved[i])*ga+(b.silent?0.0:b.interleaved[i])*gb;
  if(!std::isfinite(x)) return Result<PcmBlock>::failure({"audio_mix_nonfinite","PCM must be finite",{}});
  // Unity below the knee; continuous saturating soft knee above 0.5.
  const double v=std::abs(x); o.interleaved.push_back(static_cast<float>(std::copysign(v<=.5?v:1.0-.5*std::exp(-2*(v-.5)),x)));
 }
 o.silent=a.silent&&b.silent; o.discontinuity=a.discontinuity||b.discontinuity;
 o.timestamp_reconstructed=a.timestamp_reconstructed||b.timestamp_reconstructed;
 return Result<PcmBlock>::success(std::move(o));
}
}
