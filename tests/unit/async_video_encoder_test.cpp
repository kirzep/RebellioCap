#include "video/async_video_encoder.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rebelliocap {
struct AsyncVideoEncoderTestAccess {
  static ConvertedVideoFrame frame(std::shared_ptr<const void> lease = {}) {
    return ConvertedVideoFrame({}, std::move(lease));
  }
};
namespace {
using namespace std::chrono_literals;
struct BackendState {
  std::mutex mutex;
  std::condition_variable changed;
  bool stall{false}, entered{false}, can_submit{true}, fail_prepare{false};
  bool fail_encode{false}, fail_flush{false}, destroyed{false};
  std::vector<std::pair<QpcTicks, bool>> submissions;
  std::vector<std::thread::id> calls;
  std::uint64_t prepares{0}, flushes{0};
  void release() {
    { std::lock_guard lock(mutex); stall = false; can_submit = true; }
    changed.notify_all();
  }
  bool wait_entered() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 2s, [&] { return entered; });
  }
  bool wait_submissions(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 2s, [&] { return submissions.size() >= count; });
  }
};
class FakeBackend final : public IAsyncVideoEncoderBackend {
 public:
  explicit FakeBackend(std::shared_ptr<BackendState> state) : state_(std::move(state)) {}
  ~FakeBackend() override {
    std::lock_guard lock(state_->mutex);
    state_->calls.push_back(std::this_thread::get_id());
    state_->destroyed = true;
    state_->changed.notify_all();
  }
  Result<EncoderAdmission> prepare_frame(bool) override {
    std::unique_lock lock(state_->mutex);
    state_->calls.push_back(std::this_thread::get_id());
    ++state_->prepares;
    state_->entered = true;
    state_->changed.notify_all();
    if (!state_->changed.wait_for(lock, 2s, [&] { return !state_->stall; })) {
      return Result<EncoderAdmission>::failure({"fake.stalled", "test gate timed out", {}});
    }
    if (state_->fail_prepare) return Result<EncoderAdmission>::failure({"fake.prepare", "prepare failed", {}});
    return Result<EncoderAdmission>::success({{}, state_->can_submit});
  }
  Result<std::vector<EncodedPacket>> encode(ConvertedVideoFrame, QpcTicks pts, bool keyframe) override {
    std::lock_guard lock(state_->mutex);
    state_->calls.push_back(std::this_thread::get_id());
    if (state_->fail_encode) return Result<std::vector<EncodedPacket>>::failure({"fake.encode", "encode failed", {}});
    state_->submissions.emplace_back(pts, keyframe);
    state_->changed.notify_all();
    return Result<std::vector<EncodedPacket>>::success({{StreamKind::Video, 0, pts, pts, 1, keyframe, {}}});
  }
  Result<std::vector<EncodedPacket>> flush() override {
    std::lock_guard lock(state_->mutex);
    state_->calls.push_back(std::this_thread::get_id());
    ++state_->flushes;
    if (state_->fail_flush) return Result<std::vector<EncodedPacket>>::failure({"fake.flush", "flush failed", {}});
    return Result<std::vector<EncodedPacket>>::success({});
  }
  const StreamDescriptor& descriptor() const noexcept override { record(); return descriptor_; }
  NvencRuntimeMetrics metrics() const override { record(); NvencRuntimeMetrics result; result.slot_capacity = 6; return result; }
  std::shared_ptr<const NvencLifetimeMetrics> lifetime_metrics() const noexcept override { record(); return {}; }
 private:
  void record() const { std::lock_guard lock(state_->mutex); state_->calls.push_back(std::this_thread::get_id()); }
  std::shared_ptr<BackendState> state_;
  StreamDescriptor descriptor_{StreamKind::Video, "h264", "Video", {}, {1, 1000}, 16, 16};
};
// Always unblock the fake before the encoder's destructor, including REQUIRE failures.
struct ReleaseOnExit {
  std::shared_ptr<BackendState> state;
  ~ReleaseOnExit() { state->release(); }
};
auto make_encoder(const std::shared_ptr<BackendState>& state, std::size_t capacity = 2) {
  return AsyncVideoEncoder::create(std::make_unique<FakeBackend>(state), capacity);
}
}  // namespace

TEST_CASE("async encoder bounds all waiting leases without blocking producer", "[async_encoder]") {
  auto state = std::make_shared<BackendState>(); state->stall = true;
  auto created = make_encoder(state); REQUIRE(created.is_success());
  auto encoder = std::move(created).value();
  ReleaseOnExit release{state};
  auto lease = std::make_shared<int>(7);
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(lease), 10, false).is_success());
  REQUIRE(state->wait_entered());
  const auto begin = std::chrono::steady_clock::now();
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(lease), 20, false).is_success());
  auto rejected = encoder->prepare_frame(true);
  REQUIRE(rejected.is_success()); CHECK_FALSE(rejected.value().can_submit);
  CHECK(std::chrono::steady_clock::now() - begin < 100ms);
  CHECK(encoder->queue_metrics().queue_depth == 2);
  CHECK(encoder->queue_metrics().max_queue_depth == 2);
  CHECK(encoder->queue_metrics().dropped_frames == 1);
  CHECK(lease.use_count() == 3);
  state->release();
  REQUIRE(state->wait_submissions(2));
  auto packets = encoder->flush(); REQUIRE(packets.is_success());
  REQUIRE(packets.value().size() == 2);
  CHECK(packets.value()[0].pts == 10); CHECK(packets.value()[1].pts == 20);
  CHECK(lease.use_count() == 1);
}

TEST_CASE("async encoder preserves rejected IDR requests and elapsed PTS", "[async_encoder]") {
  auto state = std::make_shared<BackendState>(); state->stall = true;
  auto created = make_encoder(state, 1); REQUIRE(created.is_success());
  auto encoder = std::move(created).value(); ReleaseOnExit release{state};
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 11, false).is_success());
  REQUIRE(state->wait_entered());
  REQUIRE_FALSE(encoder->prepare_frame(true).value().can_submit);
  state->release(); REQUIRE(state->wait_submissions(1));
  // Wait for the worker to return admission capacity after submission.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (encoder->queue_metrics().queue_depth != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 9001, false).is_success());
  auto packets = encoder->flush(); REQUIRE(packets.is_success());
  REQUIRE(state->submissions.size() == 2);
  CHECK(state->submissions[0] == std::pair<QpcTicks, bool>{11, false});
  CHECK(state->submissions[1] == std::pair<QpcTicks, bool>{9001, true});
}

TEST_CASE("async encoder retries resource pressure with bounded leases and drains FIFO", "[async_encoder]") {
  auto state = std::make_shared<BackendState>(); state->can_submit = false;
  auto created = make_encoder(state); REQUIRE(created.is_success());
  auto encoder = std::move(created).value(); ReleaseOnExit release{state};
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 30, true).is_success());
  REQUIRE(state->wait_entered());
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 40, false).is_success());
  std::this_thread::sleep_for(30ms);
  { std::lock_guard lock(state->mutex); CHECK(state->submissions.empty()); CHECK(state->prepares < 50); }
  CHECK(encoder->queue_metrics().queue_depth == 2);
  state->release();
  auto packets = encoder->flush(); REQUIRE(packets.is_success());
  REQUIRE(packets.value().size() == 2);
  CHECK(packets.value()[0].pts == 30); CHECK(packets.value()[0].keyframe);
  CHECK(packets.value()[1].pts == 40);
  REQUIRE(encoder->flush().is_success());
  CHECK(state->flushes == 1);
}

TEST_CASE("async encoder caches snapshots and confines backend lifetime to worker", "[async_encoder]") {
  auto state = std::make_shared<BackendState>();
  auto created = make_encoder(state); REQUIRE(created.is_success());
  auto encoder = std::move(created).value();
  CHECK(encoder->descriptor().codec == "h264");
  CHECK(encoder->metrics().slot_capacity == 6);
  CHECK_FALSE(encoder->lifetime_metrics());
  REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 100, true).is_success());
  encoder.reset();
  REQUIRE(state->destroyed); REQUIRE(state->submissions.size() == 1); CHECK(state->flushes == 1);
  REQUIRE_FALSE(state->calls.empty());
  for (const auto& thread : state->calls) { CHECK(thread == state->calls.front()); CHECK(thread != std::this_thread::get_id()); }
}

TEST_CASE("async encoder propagates worker and shutdown failures", "[async_encoder]") {
  for (const int failure : {0, 1, 2}) {
    auto state = std::make_shared<BackendState>();
    state->fail_prepare = failure == 0; state->fail_encode = failure == 1; state->fail_flush = failure == 2;
    auto created = make_encoder(state); REQUIRE(created.is_success());
    auto encoder = std::move(created).value();
    REQUIRE(encoder->encode(AsyncVideoEncoderTestAccess::frame(), 5, false).is_success());
    auto flushed = encoder->flush(); REQUIRE_FALSE(flushed.is_success());
    CHECK(flushed.error().code == (failure == 0 ? "fake.prepare" : failure == 1 ? "fake.encode" : "fake.flush"));
    auto prepared = encoder->prepare_frame(false); REQUIRE_FALSE(prepared.is_success());
    CHECK(prepared.error().code == flushed.error().code);
    CHECK(encoder->queue_metrics().queue_depth == 0);
    CHECK(state->destroyed);
  }
}

TEST_CASE("async encoder rejects invalid queue configuration", "[async_encoder]") {
  auto state = std::make_shared<BackendState>();
  CHECK_FALSE(make_encoder(state, 0).is_success());
  CHECK_FALSE(make_encoder(state, 3).is_success());
}
}  // namespace rebelliocap
