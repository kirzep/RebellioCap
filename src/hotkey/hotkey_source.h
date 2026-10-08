#pragma once

#include <functional>
#include <span>

#include "core/qpc_clock.h"
#include "core/result.h"
#include "hotkey/hotkey_chord.h"

namespace rebelliocap {

using HotkeyCallback = std::function<void(HotkeyAction, QpcTicks)>;

class IHotkeySource {
 public:
  virtual ~IHotkeySource() = default;
  virtual Result<void> start(std::span<const HotkeyBinding> bindings,
                             HotkeyCallback on_press) = 0;
  virtual void stop() noexcept = 0;
};

}  // namespace rebelliocap
