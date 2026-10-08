#include "engine/session_input.h"
#include <array>
#include <atomic>
#include <deque>
#include <thread>
#include <utility>

namespace rebelliocap {
void SessionWake::notify() {
  std::scoped_lock lock(mutex_); ++revision_; changed_.notify_all();
}
std::uint64_t SessionWake::revision() const { std::scoped_lock lock(mutex_); return revision_; }
bool SessionWake::wait_until(std::uint64_t revision, std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  return changed_.wait_until(lock, deadline, [&] { return revision_ != revision; });
}
struct SessionPipeReader::Impl {
  HANDLE input;
  std::shared_ptr<SessionWake> wake;
  mutable std::mutex mutex;
  std::condition_variable space;
  std::deque<SessionInputChunk> queue;
  std::atomic_bool stopping{false};
  std::thread worker;
  static constexpr std::size_t kCapacity = 32;
  Impl(HANDLE input_value, std::shared_ptr<SessionWake> signal) : input(input_value), wake(std::move(signal)) {}
  bool push(SessionInputChunk chunk) {
    {
      std::unique_lock lock(mutex);
      space.wait(lock, [&] { return stopping.load() || queue.size() < kCapacity; });
      if (stopping.load()) return false;
      queue.push_back(std::move(chunk));
    }
    wake->notify();
    return true;
  }
  void run() {
    while (!stopping.load()) {
      std::array<char, 4096> bytes{}; DWORD count = 0;
      if (!ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
        const auto failure = GetLastError();
        if (stopping.load()) return;
        if (failure == ERROR_BROKEN_PIPE) static_cast<void>(push({{}, true, {}}));
        else static_cast<void>(push({{}, true, Error{"protocol.read_failed", "Cannot read session pipe", static_cast<long>(failure)}}));
        return;
      }
      if (count == 0) { static_cast<void>(push({{}, true, {}})); return; }
      if (!push({std::string(bytes.data(), count), false, {}})) return;
    }
  }
};
SessionPipeReader::SessionPipeReader(HANDLE input, std::shared_ptr<SessionWake> wake)
    : implementation_(std::make_unique<Impl>(input, std::move(wake))) {
  auto& state = *implementation_;
  state.worker = std::thread([&state] { state.run(); });
}
SessionPipeReader::~SessionPipeReader() {
  auto& state = *implementation_;
  state.stopping.store(true); state.space.notify_all();
  if (state.worker.joinable()) {
    // Cancellation can precede ReadFile by a few instructions. Repeat only
    // during teardown until the dedicated reader exits; idle never polls.
    const auto worker = static_cast<HANDLE>(state.worker.native_handle());
    do { static_cast<void>(CancelSynchronousIo(worker)); }
    while (WaitForSingleObject(worker, 50) == WAIT_TIMEOUT);
    state.worker.join();
  }
}
std::optional<SessionInputChunk> SessionPipeReader::try_take() {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  if (state.queue.empty()) return {};
  auto chunk = std::move(state.queue.front()); state.queue.pop_front(); state.space.notify_one();
  return chunk;
}
std::size_t SessionPipeReader::queued_chunks() const {
  auto& state = *implementation_; std::scoped_lock lock(state.mutex); return state.queue.size();
}
}
