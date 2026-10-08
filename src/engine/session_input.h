#pragma once
#include <Windows.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "core/result.h"

namespace rebelliocap {
// Producers signal without calling engine/controller code. A revision captured
// before consuming work prevents a signal during that work from being lost.
class SessionWake {
 public:
  void notify();
  std::uint64_t revision() const;
  bool wait_until(std::uint64_t revision, std::chrono::steady_clock::time_point deadline);
 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t revision_{};
};
struct SessionInputChunk {
  std::string bytes;
  bool eof{false};
  std::optional<Error> error;
};
class SessionPipeReader {
 public:
  SessionPipeReader(HANDLE input, std::shared_ptr<SessionWake> wake);
  ~SessionPipeReader();
  SessionPipeReader(const SessionPipeReader&) = delete;
  SessionPipeReader& operator=(const SessionPipeReader&) = delete;
  std::optional<SessionInputChunk> try_take();
  std::size_t queued_chunks() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};
}
