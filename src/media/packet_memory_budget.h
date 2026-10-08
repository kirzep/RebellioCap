#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/result.h"
#include "media/encoded_packet.h"

namespace rebelliocap {

// Retained compressed allocations, not codec/GPU memory or process RSS.
// Aliasing ownership keeps the vector address and never copies its bytes.
class PacketMemoryBudget {
  struct Payload {
    std::shared_ptr<void> reservation;
    std::shared_ptr<const std::vector<std::byte>> original;
  };
  struct State {
    using Entries = std::list<std::pair<const void*, std::weak_ptr<Payload>>>;
    explicit State(std::size_t maximum) : limit(maximum), sweep(entries.end()) {}
    const std::size_t limit;
    std::atomic<std::size_t> used{0};
    std::mutex mutex;
    Entries entries;
    Entries::iterator sweep;
    std::unordered_map<const void*, Entries::iterator> payloads;
  };

 public:
  static constexpr std::size_t kDefaultLimit = 64 * 1024 * 1024;
  // Covers ring + continuous descriptors, vector/control block and registry.
  static constexpr std::size_t kPayloadOverhead = 2 * sizeof(EncodedPacket) + 256;
  explicit PacketMemoryBudget(std::size_t limit) : state_(std::make_shared<State>(limit)) {}
  [[nodiscard]] std::size_t limit() const noexcept { return state_->limit; }
  [[nodiscard]] std::size_t bytes() const noexcept { return state_->used.load(); }

  static std::size_t payload_cost(const std::shared_ptr<const std::vector<std::byte>>& payload) {
    const auto capacity = payload ? payload->capacity() : 0;
    if (capacity > (std::numeric_limits<std::size_t>::max)() - kPayloadOverhead) {
      return (std::numeric_limits<std::size_t>::max)();
    }
    return capacity + kPayloadOverhead;
  }

  Result<std::shared_ptr<void>> reserve(std::size_t amount) {
    auto used = state_->used.load();
    do {
      if (amount > state_->limit || used > state_->limit - amount) {
        return Result<std::shared_ptr<void>>::failure(
            {"replay.memory_budget_exceeded", "The shared encoded-data memory budget is full. Wait for pending saves or reduce Replay duration.", {}});
      }
    } while (!state_->used.compare_exchange_weak(used, used + amount));
    // A deleter releases the charge even when shared_ptr control allocation fails.
    auto reservation = std::shared_ptr<void>(state_.get(), [state = state_, amount](void*) {
      state->used.fetch_sub(amount);
    });
    return Result<std::shared_ptr<void>>::success(std::move(reservation));
  }

  Result<std::shared_ptr<const std::vector<std::byte>>> track(
      std::shared_ptr<const std::vector<std::byte>> original) {
    using PayloadPointer = std::shared_ptr<const std::vector<std::byte>>;
    if (!original) return Result<PayloadPointer>::success(nullptr);
    std::scoped_lock lock(state_->mutex);
    // Bound maintenance work even when a long Replay retains many small packets.
    // List iterators stay valid across hash rehashes and unrelated removals.
    for (int visited = 0; visited < 8 && !state_->entries.empty(); ++visited) {
      if (state_->sweep == state_->entries.end()) state_->sweep = state_->entries.begin();
      const auto entry = state_->sweep++;
      if (entry->second.expired()) {
        state_->payloads.erase(entry->first);
        state_->entries.erase(entry);
      }
    }
    const auto found = state_->payloads.find(original.get());
    if (found != state_->payloads.end()) {
      if (auto retained = found->second->second.lock()) {
        return Result<PayloadPointer>::success(PayloadPointer(retained, original.get()));
      }
      if (state_->sweep == found->second) ++state_->sweep;
      state_->entries.erase(found->second);
      state_->payloads.erase(found);
    }
    auto allocation = reserve(payload_cost(original));
    if (!allocation.is_success()) return Result<PayloadPointer>::failure(allocation.error());
    auto retained = std::make_shared<Payload>(
        Payload{std::move(allocation).value(), std::move(original)});
    const auto address = retained->original.get();
    const auto entry = state_->entries.insert(state_->entries.end(), {address, retained});
    try {
      state_->payloads.emplace(address, entry);
    } catch (...) {
      state_->entries.erase(entry);
      throw;
    }
    return Result<PayloadPointer>::success(PayloadPointer(retained, address));
  }

 private:
  std::shared_ptr<State> state_;
};

}  // namespace rebelliocap
