#include <iostream>
#include <string_view>
#include "engine/game_category.h"

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--cache-icon") {
    return rebelliocap::cache_game_icon(argv[2], argv[3]) ? 0 : 1;
  }
  rebelliocap::MonitorInfo monitor{};
  const auto resolver = rebelliocap::GameCategoryResolver::create(monitor, std::filesystem::current_path());
  std::cout << "Local NVIDIA profiles: " << resolver->profile_count()
            << "\nGame executable candidates: " << resolver->executable_count() << '\n';
  // Targeted diagnostic lookups only: never exports the driver's catalog.
  const wchar_t* examples[] = {L"cs2.exe", L"eldenring.exe", L"cyberpunk2077.exe", L"chrome.exe", L"msedge.exe", L"discord.exe", L"photoshop.exe", L"blender.exe", L"steam.exe", L"obs64.exe", L"vlc.exe", L"winword.exe", L"excel.exe", L"acad.exe", L"3dmark.exe", L"davinciresolve.exe"};
  for (const auto* executable : examples) {
    std::wcout << executable << L": " << resolver->category_for_executable(std::filesystem::path(L"C:\\probe") / executable) << L'\n';
  }
  return 0;
}
