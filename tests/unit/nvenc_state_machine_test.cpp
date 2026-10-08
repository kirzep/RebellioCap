#include <array>
#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video/nvenc_state_machine.h"
#include "video/nvenc_decode_timeline.h"

namespace {

using rebelliocap::Error;
using rebelliocap::Result;
using rebelliocap::detail::NvencEncoderState;
using rebelliocap::detail::NvencStateMachine;
using rebelliocap::detail::NvencTrackedSlotState;

Error injected_error(std::string code) {
  return {.code = std::move(code),
          .message = "deterministic injected NVENC failure",
          .hresult = 17};
}

Result<void> success() { return Result<void>::success(); }

}  // namespace

TEST_CASE("NVENC decode timestamps retain missed frame time across B frame reorder") {
  rebelliocap::detail::NvencDecodeTimeline timeline;
  // Display sequence skips eight frames. Encoded sequence is I0 P10 B1 B2 P13 B11 B12.
  for (const auto pts : {0, 10, 20, 100, 110, 120, 130}) timeline.submit(pts, 10);
  const std::array<rebelliocap::QpcTicks, 7> presentation{0, 100, 10, 20, 130, 110, 120};
  const std::array<rebelliocap::QpcTicks, 7> expected{-20, -10, 0, 10, 20, 100, 110};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto dts = timeline.take();
    REQUIRE(dts.has_value());
    CHECK(*dts == expected[i]);
    CHECK(*dts <= presentation[i]);
  }
}

TEST_CASE("NVENC decode timestamps preserve fractional frame cadence") {
  rebelliocap::detail::NvencDecodeTimeline timeline;
  for (const auto pts : {1000000, 1166666, 1333333, 1500000}) timeline.submit(pts, 166666);
  CHECK(timeline.take() == 666668);
  CHECK(timeline.take() == 833334);
  CHECK(timeline.take() == 1000000);
  CHECK(timeline.take() == 1166666);
}

TEST_CASE("unlock failure is terminal and preserves the complete slot for cleanup") {
  NvencStateMachine machine;
  NvencTrackedSlotState slot{
      .busy = true, .completion_ready = true, .bitstream_locked = true, .mapped = true};
  int unlock_calls = 0;
  int unmap_calls = 0;

  const auto result = machine.complete_slot(
      slot,
      [&] {
        ++unlock_calls;
        return Result<void>::failure(injected_error("nvenc.injected_unlock"));
      },
      [&] {
        ++unmap_calls;
        return success();
      });

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "nvenc.injected_unlock");
  REQUIRE(machine.state() == NvencEncoderState::Failed);
  REQUIRE(machine.active_slot_count(std::array{slot}) == 1);
  REQUIRE(slot.busy);
  REQUIRE(slot.bitstream_locked);
  REQUIRE(slot.mapped);
  REQUIRE(unlock_calls == 1);
  REQUIRE(unmap_calls == 0);

  const auto cleanup = machine.cleanup_slot(
      slot,
      [&] {
        ++unlock_calls;
        return success();
      },
      [&] {
        ++unmap_calls;
        return success();
      });

  REQUIRE(cleanup.is_success());
  REQUIRE(machine.state() == NvencEncoderState::Failed);
  REQUIRE(machine.active_slot_count(std::array{slot}) == 0);
  REQUIRE_FALSE(slot.busy);
  REQUIRE_FALSE(slot.bitstream_locked);
  REQUIRE_FALSE(slot.mapped);
  REQUIRE(unlock_calls == 2);
  REQUIRE(unmap_calls == 1);
}

TEST_CASE("unmap failure is terminal and cleanup never repeats a successful unlock") {
  NvencStateMachine machine;
  NvencTrackedSlotState slot{
      .busy = true, .completion_ready = true, .bitstream_locked = true, .mapped = true};
  int unlock_calls = 0;
  int unmap_calls = 0;

  const auto result = machine.complete_slot(
      slot,
      [&] {
        ++unlock_calls;
        return success();
      },
      [&] {
        ++unmap_calls;
        return Result<void>::failure(injected_error("nvenc.injected_unmap"));
      });

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "nvenc.injected_unmap");
  REQUIRE(machine.state() == NvencEncoderState::Failed);
  REQUIRE(slot.busy);
  REQUIRE_FALSE(slot.bitstream_locked);
  REQUIRE(slot.mapped);

  const auto cleanup = machine.cleanup_slot(
      slot,
      [&] {
        ++unlock_calls;
        return success();
      },
      [&] {
        ++unmap_calls;
        return success();
      });

  REQUIRE(cleanup.is_success());
  REQUIRE(unlock_calls == 1);
  REQUIRE(unmap_calls == 2);
  REQUIRE(machine.active_slot_count(std::array{slot}) == 0);
}

TEST_CASE("drain failure after EOS closes encode and repeat flush returns the first error") {
  NvencStateMachine machine;
  int eos_calls = 0;
  int drain_calls = 0;
  int wait_calls = 0;
  const auto submit_eos = [&] {
    ++eos_calls;
    return success();
  };
  const auto drain = [&] {
    ++drain_calls;
    return Result<void>::failure(injected_error("nvenc.injected_drain"));
  };
  const auto wait = [&] {
    ++wait_calls;
    return success();
  };

  const auto first = machine.flush(submit_eos, drain, wait);
  const auto encode_after_eos = machine.require_accepting();
  const auto second = machine.flush(submit_eos, drain, wait);

  REQUIRE_FALSE(first.is_success());
  REQUIRE(first.error().code == "nvenc.injected_drain");
  REQUIRE(machine.state() == NvencEncoderState::Failed);
  REQUIRE(machine.eos_submitted());
  REQUIRE_FALSE(encode_after_eos.is_success());
  REQUIRE(encode_after_eos.error().code == "nvenc.injected_drain");
  REQUIRE_FALSE(second.is_success());
  REQUIRE(second.error().code == "nvenc.injected_drain");
  REQUIRE(eos_calls == 1);
  REQUIRE(drain_calls == 1);
  REQUIRE(wait_calls == 0);
}

TEST_CASE("EOS wait failure is stable and never submits duplicate EOS") {
  NvencStateMachine machine;
  int eos_calls = 0;
  int drain_calls = 0;
  int wait_calls = 0;
  const auto submit_eos = [&] {
    ++eos_calls;
    return success();
  };
  const auto drain = [&] {
    ++drain_calls;
    return success();
  };
  const auto wait = [&] {
    ++wait_calls;
    return Result<void>::failure(injected_error("nvenc.injected_wait"));
  };

  const auto first = machine.flush(submit_eos, drain, wait);
  const auto second = machine.flush(submit_eos, drain, wait);

  REQUIRE_FALSE(first.is_success());
  REQUIRE(first.error().code == "nvenc.injected_wait");
  REQUIRE_FALSE(second.is_success());
  REQUIRE(second.error().code == "nvenc.injected_wait");
  REQUIRE(machine.eos_submitted());
  REQUIRE(eos_calls == 1);
  REQUIRE(drain_calls == 1);
  REQUIRE(wait_calls == 1);
}

TEST_CASE("successful flush is idempotent and permanently closes encode") {
  NvencStateMachine machine;
  int eos_calls = 0;
  int drain_calls = 0;
  int wait_calls = 0;
  const auto submit_eos = [&] {
    ++eos_calls;
    return success();
  };
  const auto drain = [&] {
    ++drain_calls;
    return success();
  };
  const auto wait = [&] {
    ++wait_calls;
    return success();
  };

  REQUIRE(machine.require_accepting().is_success());
  REQUIRE(machine.flush(submit_eos, drain, wait).is_success());
  REQUIRE(machine.state() == NvencEncoderState::Flushed);
  REQUIRE_FALSE(machine.require_accepting().is_success());
  REQUIRE(machine.flush(submit_eos, drain, wait).is_success());
  REQUIRE(eos_calls == 1);
  REQUIRE(drain_calls == 1);
  REQUIRE(wait_calls == 1);
}
