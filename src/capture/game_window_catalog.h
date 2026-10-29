#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "core/result.h"
namespace rebelliocap {
struct GameWindowInfo {
  std::filesystem::path executable;
  std::string title;
  std::uint32_t process_id{};
};
// A process start time prevents a recycled PID/HWND from inheriting a session.
struct GameWindowIdentity {
  HWND window{};
  std::uint32_t process_id{};
  std::uint64_t process_started_at{};
  std::filesystem::path executable;
};
bool game_executable_equal(const std::filesystem::path&, const std::filesystem::path&) noexcept;
bool game_window_identity_equal(const GameWindowIdentity&, const GameWindowIdentity&) noexcept;
std::optional<GameWindowIdentity> inspect_game_window(HWND window);
std::optional<GameWindowIdentity> find_game_window(const std::filesystem::path& executable);
Result<std::vector<GameWindowInfo>> enumerate_game_windows();
}  // namespace rebelliocap
