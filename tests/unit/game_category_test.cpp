#include <catch2/catch_test_macros.hpp>
#include "engine/game_category.h"

using namespace rebelliocap;

TEST_CASE("Game icons are copied as valid ICO files and reused without the executable") {
  const auto directory = std::filesystem::temp_directory_path() /
      (L"scopeclipper-icon-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() { std::error_code error; std::filesystem::remove(directory / L".game-icon.ico", error); std::filesystem::remove(directory, error); }
  } cleanup{directory};
  wchar_t system[MAX_PATH]{};
  REQUIRE(GetSystemDirectoryW(system, MAX_PATH) > 0);
  CHECK_FALSE(cache_game_icon(directory / L"missing.exe", directory));
  CHECK_FALSE(std::filesystem::exists(directory / L".game-icon.ico"));
  REQUIRE(cache_game_icon(std::filesystem::path(system) / L"shell32.dll", directory));
  const auto icon_file = directory / L".game-icon.ico";
  REQUIRE(std::filesystem::file_size(icon_file) > 22);
  const auto icon = static_cast<HICON>(LoadImageW(nullptr, icon_file.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
  REQUIRE(icon != nullptr);
  DestroyIcon(icon);
  const auto modified = std::filesystem::last_write_time(icon_file);
  CHECK(cache_game_icon(directory / L"missing.exe", directory));
  CHECK(std::filesystem::last_write_time(icon_file) == modified);
}

TEST_CASE("Category names cannot escape the recording root") {
  CHECK(sanitize_category_name(L" ../Game: Test/ ") == L".._Game_ Test_");
  CHECK(sanitize_category_name(L"..") == L"Desktop");
  CHECK(sanitize_category_name(L"CON.txt") == L"_CON.txt");
  CHECK(sanitize_category_name(L"") == L"Desktop");
}
TEST_CASE("Known desktop applications and ambiguous engines are excluded") {
  CHECK(is_game_profile_candidate(L"Counter-Strike 2", L"cs2.exe"));
  CHECK_FALSE(is_game_profile_candidate(L"Adobe Photoshop", L"photoshop.exe"));
  CHECK_FALSE(is_game_profile_candidate(L"Microsoft Word", L"winword.exe"));
  CHECK_FALSE(is_game_profile_candidate(L"3DMark", L"3dmark.exe"));
  CHECK_FALSE(is_game_profile_candidate(L"Some Game", L"javaw.exe"));
  CHECK_FALSE(is_game_profile_candidate(L"Game A / Game B", L"engine.exe"));
}
TEST_CASE("Executable matching rejects ambiguity and honors explicit overrides") {
  GameCategoryIndex index;
  index.add({L"cs2.exe", L"Counter-Strike 2", {}});
  CHECK(index.category_for(L"C:\\Steam\\CS2\\CS2.EXE") == L"Counter-Strike 2");
  CHECK(index.category_for(L"C:\\Windows\\explorer.exe") == L"Desktop");
  index.add({L"cs2.exe", L"Different Game", {}});
  CHECK(index.category_for(L"C:\\Steam\\CS2\\cs2.exe") == L"Desktop");
  index.override_executable(L"CS2.EXE", L"My Game");
  CHECK(index.category_for(L"C:\\Steam\\CS2\\cs2.exe") == L"My Game");
}
TEST_CASE("Driver path and file marker constraints are required") {
  GameCategoryIndex index;
  index.add({L"C:\\Games\\One\\one.exe", L"One", {}});
  index.add({L"two.exe", L"Two", L"missing-game-marker.dat"});
  CHECK(index.category_for(L"C:\\Games\\Other\\one.exe") == L"Desktop");
  CHECK(index.category_for(L"C:\\Games\\One\\one.exe") == L"One");
  CHECK(index.category_for(L"C:\\Games\\Two\\two.exe") == L"Desktop");
}
