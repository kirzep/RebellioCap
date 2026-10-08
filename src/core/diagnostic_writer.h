#pragma once

#include <ostream>

#include "core/diagnostic_event.h"

namespace rebelliocap {

class DiagnosticWriter {
 public:
  explicit DiagnosticWriter(std::ostream& output);

  void write(const DiagnosticEvent& event);

 private:
  std::ostream& output_;
};

}  // namespace rebelliocap
