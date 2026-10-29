#include "capture/game_window_catalog.h"
#include <dwmapi.h>
#include <algorithm>
#include <exception>
#include <memory>
namespace rebelliocap {
namespace {
struct HandleCloser { void operator()(void* value) const noexcept { if (value) CloseHandle(value); } };
using ProcessHandle = std::unique_ptr<void, HandleCloser>;
std::wstring normalized_path(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()) return {};
  auto value = path.lexically_normal().native();
  std::replace(value.begin(), value.end(), L'/', L'\\');
  if (value.starts_with(L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
  else if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
  return value;
}
std::string window_title(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0 || length > 32767) return {};
  std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(window, title.data(), length + 1);
  if (copied <= 0) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, title.data(), copied, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string utf8(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, title.data(), copied, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}
bool suitable(HWND window) {
  if (!IsWindow(window) || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return false;
  if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;
  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked && !IsIconic(window)) return false;
  RECT bounds{};
  if (IsIconic(window)) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return false;
    bounds = placement.rcNormalPosition;
  } else if (!GetClientRect(window, &bounds)) return false;
  return bounds.right > bounds.left && bounds.bottom > bounds.top;
}
struct Enumeration {
  std::vector<GameWindowIdentity> identities;
  std::exception_ptr error;
};
BOOL CALLBACK enum_window(HWND window, LPARAM parameter) noexcept {
  auto& enumeration = *reinterpret_cast<Enumeration*>(parameter);
  try {
    if (auto identity = inspect_game_window(window)) enumeration.identities.push_back(std::move(*identity));
  } catch (...) { enumeration.error = std::current_exception(); return FALSE; }
  return TRUE;
}
Result<std::vector<GameWindowIdentity>> identities() {
  Enumeration enumeration;
  SetLastError(ERROR_SUCCESS);
  if (!EnumWindows(enum_window, reinterpret_cast<LPARAM>(&enumeration))) {
    return Result<std::vector<GameWindowIdentity>>::failure({"game_catalog.enumeration_failed", "Could not enumerate selectable windows.", HRESULT_FROM_WIN32(GetLastError())});
  }
  if (enumeration.error) std::rethrow_exception(enumeration.error);
  return Result<std::vector<GameWindowIdentity>>::success(std::move(enumeration.identities));
}
}  // namespace
bool game_executable_equal(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
  try {
    const auto a = normalized_path(left), b = normalized_path(right);
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
  } catch (...) { return false; }
}
bool game_window_identity_equal(const GameWindowIdentity& a, const GameWindowIdentity& b) noexcept {
  return a.window && a.process_id && a.process_started_at && a.window == b.window && a.process_id == b.process_id &&
    a.process_started_at == b.process_started_at && game_executable_equal(a.executable, b.executable);
}
std::optional<GameWindowIdentity> inspect_game_window(HWND window) {
  if (!suitable(window) || window_title(window).empty()) return std::nullopt;
  DWORD pid = 0;
  const DWORD thread = GetWindowThreadProcessId(window, &pid);
  if (!thread || !pid || pid == GetCurrentProcessId()) return std::nullopt;
  ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
  if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return std::nullopt;
  FILETIME started{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.get(), &started, &exited, &kernel, &user)) return std::nullopt;
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length) || !length) return std::nullopt;
  path.resize(length);
  DWORD current_pid = 0;
  if (GetWindowThreadProcessId(window, &current_pid) != thread || current_pid != pid || !suitable(window) ||
      WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return std::nullopt;
  const auto start = (static_cast<std::uint64_t>(started.dwHighDateTime) << 32) | started.dwLowDateTime;
  return GameWindowIdentity{window, pid, start, std::filesystem::path(std::move(path))};
}
std::optional<GameWindowIdentity> find_game_window(const std::filesystem::path& executable) {
  auto windows = identities();
  if (!windows.is_success()) return std::nullopt;
  std::optional<GameWindowIdentity> selected;
  for (const auto& identity : windows.value()) {
    if (!game_executable_equal(identity.executable, executable)) continue;
    // Persisted selection is an executable, so multiple windows are ambiguous.
    if (selected) return std::nullopt;
    selected = identity;
  }
  return selected;
}
Result<std::vector<GameWindowInfo>> enumerate_game_windows() {
  auto windows = identities();
  if (!windows.is_success()) return Result<std::vector<GameWindowInfo>>::failure(windows.error());
  std::vector<GameWindowInfo> result;
  for (const auto& window : windows.value()) {
    auto title = window_title(window.window);
    auto current = inspect_game_window(window.window);
    if (!title.empty() && current && game_window_identity_equal(window, *current))
      result.push_back({window.executable, std::move(title), window.process_id});
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.title < b.title; });
  return Result<std::vector<GameWindowInfo>>::success(std::move(result));
}
}  // namespace rebelliocap
