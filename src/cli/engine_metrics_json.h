#pragma once

#include <nlohmann/json.hpp>

#include "engine/engine_config.h"
#include "video/nvenc_encoder.h"

namespace rebelliocap {

nlohmann::json engine_metrics_json(const EngineMetrics& metrics,
                                   QpcTicks qpc_frequency,
                                   const NvencRuntimeMetrics& video_metrics);

}  // namespace rebelliocap
