#include "engine/game_category.h"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <set>
#include <string_view>
#include <nlohmann/json.hpp>
#include "nvapi.h"
#include "nvapi_interface.h"

namespace rebelliocap {
namespace {
std::wstring lower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return text;
}
std::wstring unicode(const NvAPI_UnicodeString& text) {
  std::wstring result;
  for (const auto c : text) { if (!c) break; result.push_back(static_cast<wchar_t>(c)); }
  return result;
}
template <typename T> T api(FARPROC query, const char* name) {
  using Query = void* (__cdecl*)(unsigned int);
  for (const auto& entry : nvapi_interface_table)
    if (std::string_view(entry.func) == name) return reinterpret_cast<T>(reinterpret_cast<Query>(query)(entry.id));
  return nullptr;
}
bool marker_matches(const std::filesystem::path& exe, const std::wstring& markers) {
  std::size_t start = 0;
  while (start < markers.size()) {
    const auto end = markers.find(L':', start);
    const auto marker = std::filesystem::path(markers.substr(start, end - start));
    // Never let driver-provided markers escape the executable directory.
    if (marker.empty() || marker.is_absolute() || marker.has_parent_path()) return false;
    std::error_code error;
    if (!std::filesystem::exists(exe.parent_path() / marker, error) || error) return false;
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return true;
}
}

std::wstring sanitize_category_name(std::wstring name) {
  for (auto& c : name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
  while (!name.empty() && (name.back() == L'.' || std::iswspace(name.back()))) name.pop_back();
  while (!name.empty() && std::iswspace(name.front())) name.erase(name.begin());
  if (name.empty() || name == L"." || name == L"..") return L"Desktop";
  const auto base = lower(name.substr(0, name.find(L'.')));
  if (lower(name) == L"desktop") return L"Desktop";
  if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
      (base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt")) && base.back() >= L'1' && base.back() <= L'9')) name.insert(name.begin(), L'_');
  if (name.size() > 100) name.resize(100);
  while (!name.empty() && (name.back() == L'.' || std::iswspace(name.back()))) name.pop_back();
  return name;
}

bool is_game_profile_candidate(const std::wstring& title, const std::wstring& executable) {
  const auto name = lower(title);
  const auto exe = lower(executable);
  if (name.empty() || name.find(L" / ") != std::wstring::npos || name.find(L"; ") != std::wstring::npos) return false;
  constexpr std::wstring_view excluded[] = {
    L"adobe", L"autodesk", L"photoshop", L"premiere", L"after effects", L"illustrator", L"acrobat",
    L"chrome", L"firefox", L"browser", L"microsoft edge", L"internet explorer", L"discord", L"skype", L"zoom",
    L"microsoft office", L"microsoft outlook", L"microsoft word", L"microsoft access", L"microsoft publisher", L"onenote", L"microsoft teams", L"powerpoint", L"microsoft excel", L"wordpad", L"visual studio", L"blender", L"autodesk maya",
    L"cinema 4d", L"3ds max", L"solidworks", L"catia", L"ansys", L"davinci", L"fusion 360", L"houdini", L"arcgis", L"siemens nx", L"paraview", L"keyshot", L"lumion", L"sketchup", L"ptc creo",
    L"benchmark", L"3dmark", L"pcmark", L"furmark", L"cinebench", L"unigine", L"passmark", L"geekbench",
    L"launcher", L"steam client", L"epic games", L"obs studio", L"video player", L"vlc", L"media player",
    L"desktop", L"windows explorer", L"remote desktop", L"nvidia", L"unity editor", L"unreal editor",
    L"paint.net", L"microsoft paint", L"gimp", L"wpf", L"opengl test", L"cuda", L"application profile"};
  for (const auto word : excluded) if (name.find(word) != std::wstring::npos) return false;
  constexpr std::wstring_view blocked[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"discord.exe", L"steam.exe", L"explorer.exe", L"obs64.exe", L"blender.exe", L"unity.exe", L"unrealeditor.exe", L"java.exe", L"javaw.exe", L"python.exe", L"pythonw.exe", L"application.exe", L"launcher.exe"};
  for (const auto word : blocked) if (exe == word) return false;
  return exe.ends_with(L".exe");
}

void GameCategoryIndex::add(GameProfileEntry entry) { entries_.push_back(std::move(entry)); }
void GameCategoryIndex::override_executable(std::wstring executable, std::wstring title) {
  overrides_.push_back({lower(std::move(executable)), sanitize_category_name(std::move(title)), {}});
}
std::wstring GameCategoryIndex::category_for(const std::filesystem::path& executable) const {
  const auto filename = lower(executable.filename().wstring());
  for (const auto& rule : overrides_) if (rule.executable == filename) return rule.title;
  std::set<std::wstring> matches;
  for (const auto& entry : entries_) {
    const std::filesystem::path candidate(entry.executable);
    if (lower(candidate.filename().wstring()) != filename) continue;
    if (candidate.is_absolute() && lower(candidate.wstring()) != lower(executable.wstring())) continue;
    if (!candidate.is_absolute() && candidate.has_parent_path()) {
      const auto full = lower(executable.wstring());
      const auto suffix = lower(candidate.wstring());
      if (!full.ends_with(L"\\" + suffix)) continue;
    }
    if (!entry.file_in_folder.empty() && !marker_matches(executable, entry.file_in_folder)) continue;
    matches.insert(sanitize_category_name(entry.title));
  }
  return matches.size() == 1 ? *matches.begin() : L"Desktop";
}

std::shared_ptr<GameCategoryResolver> GameCategoryResolver::create(const MonitorInfo& monitor, const std::filesystem::path& output_root) {
  auto result = std::shared_ptr<GameCategoryResolver>(new GameCategoryResolver());
  result->monitor_rect_ = monitor.desktop_rect;
  result->output_root_ = output_root;
  try { result->load_driver(); } catch (...) { result->index_ = {}; result->profile_count_ = 0; }
  try { result->load_overrides(); } catch (...) { /* A malformed optional rule file cannot interrupt recording. */ }
  return result;
}

void GameCategoryResolver::load_driver() {
  const HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) return;
  struct ModuleGuard { HMODULE value; ~ModuleGuard() { FreeLibrary(value); } } module_guard{module};
  const auto query = GetProcAddress(module, "nvapi_QueryInterface");
  if (!query) return;
  const auto initialize = api<decltype(&NvAPI_Initialize)>(query, "NvAPI_Initialize");
  const auto unload = api<decltype(&NvAPI_Unload)>(query, "NvAPI_Unload");
  const auto create = api<decltype(&NvAPI_DRS_CreateSession)>(query, "NvAPI_DRS_CreateSession");
  const auto destroy = api<decltype(&NvAPI_DRS_DestroySession)>(query, "NvAPI_DRS_DestroySession");
  const auto load = api<decltype(&NvAPI_DRS_LoadSettings)>(query, "NvAPI_DRS_LoadSettings");
  const auto profiles = api<decltype(&NvAPI_DRS_EnumProfiles)>(query, "NvAPI_DRS_EnumProfiles");
  const auto info = api<decltype(&NvAPI_DRS_GetProfileInfo)>(query, "NvAPI_DRS_GetProfileInfo");
  const auto applications = api<decltype(&NvAPI_DRS_EnumApplications)>(query, "NvAPI_DRS_EnumApplications");
  if (!initialize || !unload || !create || !destroy || !load || !profiles || !info || !applications || initialize() != NVAPI_OK) return;
  struct ApiGuard { decltype(&NvAPI_Unload) fn; ~ApiGuard() { fn(); } } api_guard{unload};
  NvDRSSessionHandle session{};
  if (create(&session) != NVAPI_OK) return;
  struct SessionGuard { decltype(&NvAPI_DRS_DestroySession) fn; NvDRSSessionHandle handle; ~SessionGuard() { fn(handle); } } session_guard{destroy, session};
  if (load(session) != NVAPI_OK) return;
  for (NvU32 i = 0; i < 50000; ++i) {
    NvDRSProfileHandle profile{};
    if (profiles(session, i, &profile) != NVAPI_OK) break;
    ++profile_count_;
    NVDRS_PROFILE details{}; details.version = NVDRS_PROFILE_VER;
    if (info(session, profile, &details) != NVAPI_OK || !details.isPredefined || !details.gpuSupport.geforce) continue;
    const auto title = unicode(details.profileName);
    for (NvU32 a = 0; a < details.numOfApps && a < 10000; ++a) {
      NVDRS_APPLICATION application{}; application.version = NVDRS_APPLICATION_VER;
      NvU32 count = 1;
      if (applications(session, profile, a, &count, &application) != NVAPI_OK || count != 1) break;
      const auto executable = unicode(application.appName);
      if (!application.isPredefined || application.isCommandLine || !is_game_profile_candidate(title, lower(std::filesystem::path(executable).filename().wstring()))) continue;
      const auto markers = unicode(application.fileInFolder);
      const auto basename = lower(std::filesystem::path(executable).filename().wstring());
      if (markers.empty() && (basename == L"game.exe" || basename == L"engine.exe" || basename == L"hl2.exe")) continue;
      index_.add({executable, title, markers});
    }
  }
}

void GameCategoryResolver::load_overrides() {
  std::error_code file_error;
  const auto bytes = std::filesystem::file_size(output_root_ / L"game-rules.json", file_error);
  if (file_error || bytes > 1024 * 1024) return;
  std::ifstream input(output_root_ / L"game-rules.json");
  if (!input) return;
  const auto rules = nlohmann::json::parse(input);
  if (!rules.is_object()) return;
  for (const auto& [exe, title] : rules.items()) {
    if (!title.is_string()) continue;
    if (exe.empty() || exe.size() > 260 || exe.find_first_of("/\\:") != std::string::npos || !lower(std::filesystem::path(exe).wstring()).ends_with(L".exe")) continue;
    const auto value = title.get<std::string>();
    auto wide = [](const std::string& utf8) {
      const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
      std::wstring result(static_cast<std::size_t>(size), L'\0');
      if (size > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
      return result;
    };
    index_.override_executable(wide(exe), wide(value));
  }
}

std::filesystem::path GameCategoryResolver::foreground_folder() const {
  try {
    const auto window = GetForegroundWindow();
    RECT rect{};
    if (!window || !GetWindowRect(window, &rect)) return std::filesystem::path(L"Desktop");
    const POINT center{rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2};
    const auto monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    MONITORINFO info{}; info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !EqualRect(&info.rcMonitor, &monitor_rect_)) return std::filesystem::path(L"Desktop");
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::filesystem::path(L"Desktop");
    struct HandleGuard { HANDLE value; ~HandleGuard() { CloseHandle(value); } } guard{process};
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return std::filesystem::path(L"Desktop");
    path.resize(length);
    const std::filesystem::path category(index_.category_for(path));
    if (category != L"Desktop") {
      std::lock_guard lock(icon_mutex_);
      category_executables_.try_emplace(category, path);
    }
    return category;
  } catch (...) { return std::filesystem::path(L"Desktop"); }
}
void GameCategoryResolver::cache_folder_icon(const std::filesystem::path& category) const {
  std::filesystem::path executable;
  {
    std::lock_guard lock(icon_mutex_);
    const auto found = category_executables_.find(category);
    if (found == category_executables_.end()) return;
    executable = found->second;
  }
  cache_game_icon(executable, output_root_ / category);
}
}  // namespace rebelliocap
