#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "core/result.h"

namespace rebelliocap::detail {

enum class NvencEncoderState { Accepting, EosSubmitted, Flushed, Failed };

struct NvencTrackedSlotState {
  bool busy{false};
  bool completion_ready{false};
  bool bitstream_locked{false};
  bool mapped{false};
};

class NvencStateMachine {
 public:
  [[nodiscard]] NvencEncoderState state() const noexcept { return state_; }
  [[nodiscard]] bool eos_submitted() const noexcept { return eos_submitted_; }

  [[nodiscard]] Result<void> require_accepting() const {
    if (state_ == NvencEncoderState::Accepting) {
      return Result<void>::success();
    }
    if (state_ == NvencEncoderState::Failed) {
      return stable_failure();
    }
    return Result<void>::failure(state_error(
        state_ == NvencEncoderState::Flushed ? "nvenc.already_flushed"
                                             : "nvenc.eos_submitted",
        state_ == NvencEncoderState::Flushed
            ? "Cannot submit frames after NVENC flush."
            : "Cannot submit frames after NVENC EOS submission."));
  }

  Result<void> fail(Error error) {
    if (state_ != NvencEncoderState::Failed) {
      terminal_error_ = std::move(error);
      state_ = NvencEncoderState::Failed;
    }
    return stable_failure();
  }

  template <typename Unlock, typename Unmap>
  Result<void> complete_slot(NvencTrackedSlotState& slot, Unlock&& unlock,
                             Unmap&& unmap) {
    auto released = release_slot(slot, std::forward<Unlock>(unlock),
                                 std::forward<Unmap>(unmap));
    if (!released.is_success()) {
      return fail(std::move(released).error());
    }
    return Result<void>::success();
  }

  template <typename Unlock, typename Unmap>
  Result<void> cleanup_slot(NvencTrackedSlotState& slot, Unlock&& unlock,
                            Unmap&& unmap) const {
    return release_slot(slot, std::forward<Unlock>(unlock),
                        std::forward<Unmap>(unmap));
  }

  template <typename SubmitEos, typename Drain, typename WaitEos>
  Result<void> flush(SubmitEos&& submit_eos, Drain&& drain,
                     WaitEos&& wait_eos) {
    if (state_ == NvencEncoderState::Failed) {
      return stable_failure();
    }
    if (state_ == NvencEncoderState::Flushed) {
      return Result<void>::success();
    }
    if (state_ == NvencEncoderState::Accepting) {
      auto submitted = submit_eos();
      if (!submitted.is_success()) {
        return fail(std::move(submitted).error());
      }
      eos_submitted_ = true;
      state_ = NvencEncoderState::EosSubmitted;
    }

    auto drained = drain();
    if (!drained.is_success()) {
      return fail(std::move(drained).error());
    }
    auto waited = wait_eos();
    if (!waited.is_success()) {
      return fail(std::move(waited).error());
    }
    state_ = NvencEncoderState::Flushed;
    return Result<void>::success();
  }

  [[nodiscard]] std::size_t active_slot_count(
      std::span<const NvencTrackedSlotState> slots) const noexcept {
    std::size_t count = 0;
    for (const auto& slot : slots) {
      count += slot_is_active(slot) ? 1U : 0U;
    }
    return count;
  }

  [[nodiscard]] static bool slot_is_active(
      const NvencTrackedSlotState& slot) noexcept {
    return slot.busy || slot.mapped || slot.bitstream_locked;
  }

 private:
  template <typename Unlock, typename Unmap>
  static Result<void> release_slot(NvencTrackedSlotState& slot,
                                   Unlock&& unlock, Unmap&& unmap) {
    if (slot.bitstream_locked) {
      auto unlocked = unlock();
      if (!unlocked.is_success()) {
        return unlocked;
      }
      slot.bitstream_locked = false;
    }
    if (slot.mapped) {
      auto unmapped = unmap();
      if (!unmapped.is_success()) {
        return unmapped;
      }
      slot.mapped = false;
    }
    slot.completion_ready = false;
    slot.busy = false;
    return Result<void>::success();
  }

  [[nodiscard]] Result<void> stable_failure() const {
    if (terminal_error_.has_value()) {
      return Result<void>::failure(*terminal_error_);
    }
    return Result<void>::failure(
        state_error("nvenc.failed", "The NVENC encoder is in a terminal state."));
  }

  static Error state_error(std::string code, std::string message) {
    return {.code = std::move(code),
            .message = std::move(message),
            .hresult = std::nullopt};
  }

  NvencEncoderState state_{NvencEncoderState::Accepting};
  bool eos_submitted_{false};
  std::optional<Error> terminal_error_;
};

}  // namespace rebelliocap::detail
