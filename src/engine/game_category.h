#pragma once

#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "capture/monitor_catalog.h"

namespace rebelliocap {

std::wstring sanitize_category_name(std::wstring name);
bool is_game_profile_candidate(const std::wstring& title, const std::wstring& executable);
// Copies an embedded icon resource as ICO data without executing the source binary.
bool cache_game_icon(const std::filesystem::path& executable,
                     const std::filesystem::path& directory) noexcept;

struct GameProfileEntry {
  std::wstring executable;
  std::wstring title;
  std::wstring file_in_folder;
};

class GameCategoryIndex {
 public:
  void add(GameProfileEntry entry);
  void override_executable(std::wstring executable, std::wstring title);
  std::wstring category_for(const std::filesystem::path& executable) const;
  std::size_t size() const { return entries_.size(); }
 private:
  std::vector<GameProfileEntry> entries_;
  std::vector<GameProfileEntry> overrides_;
};

// Reads only the local driver catalog once. Foreground lookup never changes driver settings.
class GameCategoryResolver {
 public:
  static std::shared_ptr<GameCategoryResolver> create(const MonitorInfo& monitor,
                                                     const std::filesystem::path& output_root);
  std::filesystem::path foreground_folder() const;
  void cache_folder_icon(const std::filesystem::path& category) const;
  std::wstring category_for_executable(const std::filesystem::path& executable) const {
    return index_.category_for(executable);
  }
  std::size_t profile_count() const { return profile_count_; }
  std::size_t executable_count() const { return index_.size(); }
 private:
  RECT monitor_rect_{};
  std::filesystem::path output_root_;
  GameCategoryIndex index_;
  std::size_t profile_count_ = 0;
  mutable std::mutex icon_mutex_;
  mutable std::map<std::filesystem::path, std::filesystem::path> category_executables_;
  void load_driver();
  void load_overrides();
};
}  // namespace rebelliocap
