#pragma once
#include "audio/audio_capture_source.h"
namespace rebelliocap {
class AudioMixer { public: Result<PcmBlock> mix(const PcmBlock&,const PcmBlock&,float,float); };
}
