#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/qpc_clock.h"

namespace rebelliocap {

struct DiagnosticEvent {
  std::string type;
  QpcTicks qpc;
  nlohmann::json fields = nlohmann::json::object();
};

}  // namespace rebelliocap
