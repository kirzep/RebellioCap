#pragma once
#include <Windows.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace rebelliocap {
struct NameContext {
 SYSTEMTIME time{};
 std::wstring game{L"Desktop"};
 std::uint64_t counter{1};
 bool recording{false};
 std::uint32_t width{1920},height{1080},fps{60};
};
std::wstring format_recording_name(const nlohmann::json& parts,const NameContext& context);
class RecordingNames {
 public:
 explicit RecordingNames(std::filesystem::path settings);
 // Disk access belongs to setup/control/save workers, never the hotkey callback.
 void reload();
 void persist_counter() noexcept;
 std::wstring capture(NameContext context);
 private:
 std::filesystem::path settings_;
 std::uint64_t counter_{0};
 nlohmann::json parts_;
 std::mutex mutex_;
 std::mutex persistence_mutex_;
 std::uint64_t persisted_counter_{0};
};
}
