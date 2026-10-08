#pragma once

#include <chrono>
#include <cstdint>

namespace rebelliocap {

using QpcTicks = std::int64_t;

class QpcClock {
 public:
  QpcClock() noexcept;

  [[nodiscard]] QpcTicks now() const noexcept;
  [[nodiscard]] QpcTicks frequency() const noexcept;
  [[nodiscard]] std::chrono::nanoseconds to_duration(QpcTicks ticks) const noexcept;

 private:
  QpcTicks frequency_;
};

}  // namespace rebelliocap
