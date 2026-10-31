#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "capture/gpu_recording_priority.h"
using namespace rebelliocap;
TEST_CASE("recording priority requests bounded absolute priority without trusting relative readback") {
  std::vector<int> requests;
  int reads = 0;
  const auto result = configure_recording_priority(true,
      [&](int value) { requests.push_back(value); return S_OK; },
      [&](int* value) { ++reads; *value = 0; return S_OK; });
  CHECK(requests == std::vector<int>{0x40000010});
  CHECK(result.applied);
  CHECK(reads == 0);
  CHECK(result.mode == GpuRecordingPriorityMode::SoftRealtime);
  CHECK_FALSE(result.effective_available);
  CHECK_FALSE(result.hresult);
}
TEST_CASE("recording priority remains opt in") {
  int calls = 0;
  const auto disabled = configure_recording_priority(false,
      [&](int) { ++calls; return S_OK; }, [&](int*) { ++calls; return S_OK; });
  CHECK(calls == 0);
  CHECK_FALSE(disabled.requested);
  CHECK_FALSE(disabled.applied);
  CHECK(disabled.mode == GpuRecordingPriorityMode::Disabled);
}
TEST_CASE("denied absolute recording priority retains its error and reports relative fallback") {
  std::vector<int> requests;
  const auto result = configure_recording_priority(true,
      [&](int value) { requests.push_back(value); return value == 7 ? S_OK : E_ACCESSDENIED; },
      [](int* value) { *value = 7; return S_OK; });
  CHECK(requests == std::vector<int>{0x40000010, 7});
  CHECK(result.requested);
  CHECK_FALSE(result.applied);
  CHECK(result.mode == GpuRecordingPriorityMode::RelativeFallback);
  CHECK(result.effective_available);
  CHECK(result.effective == 7);
  CHECK(result.hresult == static_cast<long>(E_ACCESSDENIED));
}
TEST_CASE("unavailable recording priority never invents an effective level") {
  SECTION("both setter requests fail") {
    const auto result = configure_recording_priority(true, [](int) { return E_ACCESSDENIED; },
        [](int*) { CHECK(false); return S_OK; });
    CHECK_FALSE(result.applied);
    CHECK_FALSE(result.effective_available);
  }
  SECTION("relative readback fails") {
    const auto result = configure_recording_priority(true,
        [](int value) { return value == 7 ? S_OK : E_INVALIDARG; }, [](int*) { return E_FAIL; });
    CHECK_FALSE(result.applied);
    CHECK_FALSE(result.effective_available);
    CHECK(result.hresult == static_cast<long>(E_INVALIDARG));
  }
  SECTION("relative request was ignored") {
    const auto result = configure_recording_priority(true,
        [](int value) { return value == 7 ? S_OK : E_ACCESSDENIED; },
        [](int* value) { *value = 0; return S_OK; });
    CHECK_FALSE(result.applied);
    CHECK(result.effective_available);
    CHECK(result.effective == 0);
  }
}
