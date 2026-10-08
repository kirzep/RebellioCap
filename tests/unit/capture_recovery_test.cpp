#include <catch2/catch_test_macros.hpp>
#include "capture/recovering_capture_source.h"
using namespace rebelliocap;
using namespace std::chrono_literals;
namespace {
class FakeCapture final : public ICaptureSource {
 public:
  explicit FakeCapture(std::string failure = {}) : failure_(std::move(failure)) {}
  Result<std::optional<CapturedVideoFrame>> next_frame(std::chrono::milliseconds) override {
    if (!failure_.empty()) return Result<std::optional<CapturedVideoFrame>>::failure({failure_, "test", {}});
    return Result<std::optional<CapturedVideoFrame>>::success(CapturedVideoFrame{{}, 42, 100, 100, {}});
  }
 private: std::string failure_;
};
}
TEST_CASE("Capture waits for the desktop and resumes without restarting the engine") {
  auto now = std::chrono::steady_clock::time_point{};
  int attempts = 0;
  std::vector<std::string> events;
  RecoveringCaptureSource capture(std::make_unique<FakeCapture>("dxgi_capture.access_lost"),
    [&]() -> RecoveringCaptureSource::SourceResult {
      ++attempts;
      if (attempts == 1) return RecoveringCaptureSource::SourceResult::failure({"dxgi_capture.desktop_access_denied", "locked", {}});
      return RecoveringCaptureSource::SourceResult::success(std::make_unique<FakeCapture>());
    }, [&](const std::string& event, const Error&) { events.push_back(event); }, [&] { return now; });
  REQUIRE(capture.next_frame(0ms).is_success());
  REQUIRE(capture.recovering());
  for (int n=0;n<100;n++) REQUIRE(capture.next_frame(0ms).is_success());
  REQUIRE(attempts == 0);
  now += 1s;
  REQUIRE(capture.next_frame(0ms).is_success());
  REQUIRE(attempts == 1);
  now += 1s;
  auto resumed = capture.next_frame(0ms);
  REQUIRE(resumed.is_success());
  REQUIRE(resumed.value().has_value());
  REQUIRE_FALSE(capture.recovering());
  REQUIRE(events.front() == "capture.suspended");
  REQUIRE(events.back() == "capture.restored");
}
TEST_CASE("Unrelated and device failures are never hidden by capture recovery") {
  RecoveringCaptureSource capture(std::make_unique<FakeCapture>("dxgi_capture.invalid_texture"),
    [] { return RecoveringCaptureSource::SourceResult::success(std::make_unique<FakeCapture>()); });
  REQUIRE_FALSE(capture.next_frame(0ms).is_success());
  REQUIRE_FALSE(capture.recovering());
}
TEST_CASE("A fatal recreation failure escapes instead of retrying forever") {
  auto now = std::chrono::steady_clock::time_point{};
  RecoveringCaptureSource capture(std::make_unique<FakeCapture>("dxgi_capture.access_lost"),
    [] { return RecoveringCaptureSource::SourceResult::failure({"dxgi_capture.adapter_mismatch", "fatal", {}}); }, {}, [&] {return now;});
  REQUIRE(capture.next_frame(0ms).is_success());
  now += 1s;
  auto result = capture.next_frame(0ms);
  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "dxgi_capture.adapter_mismatch");
}
TEST_CASE("Long desktop unavailability stays bounded and fatal errors remain distinct") {
  REQUIRE(is_transient_capture_error({"dxgi_capture.session_disconnected", "", {}}));
  REQUIRE(is_transient_capture_error({"dxgi_capture.output_not_attached", "", {}}));
  REQUIRE_FALSE(is_transient_capture_error({"dxgi_capture.session_limit", "", {}}));
  auto now = std::chrono::steady_clock::time_point{};
  int attempts = 0, notifications = 0;
  RecoveringCaptureSource capture(std::make_unique<FakeCapture>("dxgi_capture.access_lost"),
    [&] { ++attempts; return RecoveringCaptureSource::SourceResult::failure({"capture.monitor_unavailable", "", {}}); },
    [&](const std::string&, const Error&) { ++notifications; }, [&] {return now;});
  REQUIRE(capture.next_frame(0ms).is_success());
  for (int seconds=0; seconds<3600; ++seconds) {
    now += 1s;
    auto frame = capture.next_frame(0ms);
    REQUIRE(frame.is_success());
    REQUIRE_FALSE(frame.value().has_value());
  }
  REQUIRE(attempts == 3600);
  REQUIRE(notifications < 125);
}
