#include <chrono>
#include <memory>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/diagnostic_writer.h"
#include "core/qpc_clock.h"
#include "core/result.h"
#include "media/encoded_packet.h"

TEST_CASE("diagnostic event is one valid JSON line") {
  std::ostringstream output;
  rebelliocap::DiagnosticWriter writer(output);
  writer.write({.type = "engine.ready", .qpc = 42, .fields = {{"fps", 60}}});

  const auto serialized = output.str();
  const auto parsed = nlohmann::json::parse(serialized);
  REQUIRE(parsed["schema"] == 1);
  REQUIRE(parsed["type"] == "engine.ready");
  REQUIRE(parsed["qpc"] == 42);
  REQUIRE(parsed["fields"]["fps"] == 60);
  REQUIRE(serialized.back() == '\n');
}

TEST_CASE("diagnostics redact pointers and absolute local paths") {
  std::ostringstream output;
  rebelliocap::DiagnosticWriter writer(output);
  writer.write({.type = "capture.failed",
                .qpc = 7,
                .fields = {{"address", "0x00007FFEA1B2C3D4"},
                           {"path", "C:\\Users\\operator\\clip.mp4"},
                           {"file", "clip.mp4"}}});

  const auto parsed = nlohmann::json::parse(output.str());
  REQUIRE(parsed["fields"]["address"] == "[redacted]");
  REQUIRE(parsed["fields"]["path"] == "[redacted]");
  REQUIRE(parsed["fields"]["file"] == "clip.mp4");
}

TEST_CASE("diagnostics redact rooted and device-style Windows paths") {
  std::ostringstream output;
  rebelliocap::DiagnosticWriter writer(output);
  writer.write({.type = "capture.failed",
                .qpc = 8,
                .fields = {{"rooted", "\\Users\\operator\\clip.mp4"},
                           {"device", "\\??\\C:\\Users\\operator\\clip.mp4"}}});

  const auto parsed = nlohmann::json::parse(output.str());
  REQUIRE(parsed["fields"]["rooted"] == "[redacted]");
  REQUIRE(parsed["fields"]["device"] == "[redacted]");
  REQUIRE(output.str().find("Users") == std::string::npos);
}

TEST_CASE("diagnostics redact sensitive field keys with deterministic collisions") {
  std::ostringstream output;
  rebelliocap::DiagnosticWriter writer(output);
  writer.write({.type = "capture.failed",
                .qpc = 9,
                .fields = {{"\\Users\\operator\\clip.mp4", 1},
                           {"\\??\\C:\\Users\\operator\\clip.mp4", 2},
                           {"0x00007FFEA1B2C3D4", 3},
                           {"[redacted]", 4}}});

  const auto serialized = output.str();
  const auto parsed = nlohmann::json::parse(serialized);
  REQUIRE(parsed["fields"].size() == 4);
  REQUIRE(parsed["fields"].contains("[redacted]"));
  REQUIRE(parsed["fields"].contains("[redacted]#2"));
  REQUIRE(parsed["fields"].contains("[redacted]#3"));
  REQUIRE(parsed["fields"].contains("[redacted]#4"));
  REQUIRE(serialized.find("Users") == std::string::npos);
  REQUIRE(serialized.find("0x00007FFEA1B2C3D4") == std::string::npos);
}

TEST_CASE("QPC ticks convert to nanoseconds") {
  const rebelliocap::QpcClock clock;
  const auto frequency = clock.frequency();

  REQUIRE(frequency > 0);
  REQUIRE(clock.to_duration(frequency) == std::chrono::seconds{1});
}

TEST_CASE("result carries a move-only success value") {
  auto result = rebelliocap::Result<std::unique_ptr<int>>::success(
      std::make_unique<int>(60));

  REQUIRE(result.is_success());
  REQUIRE(*result.value() == 60);
}

TEST_CASE("result void carries an error") {
  auto result = rebelliocap::Result<void>::failure(
      {.code = "capture.unavailable", .message = "No output selected", .hresult = 5});

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "capture.unavailable");
  REQUIRE(result.error().hresult == 5);
}

TEST_CASE("encoded packet shares immutable payload") {
  auto mutable_payload = std::make_shared<std::vector<std::byte>>(4);
  const rebelliocap::EncodedPacket packet{
      .stream = rebelliocap::StreamKind::Video,
      .epoch = 1,
      .pts = 100,
      .dts = 90,
      .duration = 10,
      .keyframe = true,
      .payload = mutable_payload};

  REQUIRE(packet.payload->size() == 4);
}
