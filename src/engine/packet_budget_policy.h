#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include "config/recording_contract.h"
#include "core/result.h"
namespace rebelliocap {
struct PhysicalMemory {
  std::uint64_t total_bytes;
  std::uint64_t available_bytes;
};
// The shared budget charges retained encoded packets, never GPU memory or RSS.
inline Result<std::size_t> packet_budget_policy(std::uint32_t requested_mb,
                                               std::optional<PhysicalMemory> memory) {
  constexpr std::uint64_t MiB=1024ULL*1024;
  if(memory && (memory->total_bytes==0)) memory.reset();
  if(requested_mb==0) {
    const auto bytes=memory?(std::min)({1024*MiB,memory->available_bytes/8,memory->total_bytes/16}):64*MiB;
    return Result<std::size_t>::success(static_cast<std::size_t>(bytes));
  }
  const auto& range=recording_contract().at("ranges").at("replay_memory_limit_mb");
  if(requested_mb<range.at("min").get<std::uint32_t>() || requested_mb>range.at("max").get<std::uint32_t>())
    return Result<std::size_t>::failure({"config.invalid",recording_contract().at("messages").at("replay_memory_limit_mb").get<std::string>(),{}});
  if(!memory)
    return Result<std::size_t>::failure({"config.replay_memory_unavailable","Не удалось проверить объём доступной оперативной памяти (RAM) для лимита повтора. Выберите «Авто» или повторите попытку, когда сведения о памяти будут доступны.",{}});
  const auto allowed=(std::min)(memory->available_bytes/2,memory->total_bytes/4);
  const auto requested=static_cast<std::uint64_t>(requested_mb)*MiB;
  if(requested>allowed)
    return Result<std::size_t>::failure({"config.replay_memory_unavailable",
      "Запрошенный лимит повтора: "+std::to_string(requested_mb)+" МиБ; доступно оперативной памяти (RAM): "+std::to_string(memory->available_bytes/MiB)+
      " МиБ; допустимый лимит повтора: "+std::to_string(allowed/MiB)+" МиБ (не больше половины доступной и четверти общей памяти). Уменьшите лимит или выберите «Авто».",{}});
  return Result<std::size_t>::success(static_cast<std::size_t>(requested));
}
}
