#pragma once

#include <span>
#include <utility>
#include <vector>

#include "hotkey/hotkey_source.h"

namespace rebelliocap::testing {

class FakeHotkeySource final : public IHotkeySource {
 public:
  Result<void> start(std::span<const HotkeyBinding> bindings,
                     HotkeyCallback callback) override {
    bindings_.assign(bindings.begin(), bindings.end());
    callback_ = std::move(callback);
    return Result<void>::success();
  }

  void stop() noexcept override {
    bindings_.clear();
    callback_ = {};
  }

  void press(HotkeyAction action, QpcTicks timestamp) const {
    if (callback_) {
      callback_(action, timestamp);
    }
  }

  void press(QpcTicks timestamp) const {
    if (!bindings_.empty()) {
      press(bindings_.front().action, timestamp);
    }
  }

  [[nodiscard]] const std::vector<HotkeyBinding>& bindings() const noexcept {
    return bindings_;
  }

 private:
  std::vector<HotkeyBinding> bindings_;
  HotkeyCallback callback_;
};

}  // namespace rebelliocap::testing
