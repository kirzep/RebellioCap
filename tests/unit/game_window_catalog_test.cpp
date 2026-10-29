#include <catch2/catch_test_macros.hpp>
#include "capture/game_window_catalog.h"
using namespace rebelliocap;
TEST_CASE("Selected executable identity uses the full path and Windows case rules") {
  REQUIRE(game_executable_equal(L"C:/Games/Arena/Game.EXE", L"c:\\games\\arena\\game.exe"));
  REQUIRE_FALSE(game_executable_equal(L"C:\\Games\\Arena\\game.exe", L"D:\\Other\\game.exe"));
  REQUIRE_FALSE(game_executable_equal(L"game.exe", L"game.exe"));
  REQUIRE_FALSE(game_executable_equal({}, {}));
}
TEST_CASE("Recycled HWNDs and PIDs cannot inherit a selected game identity") {
  const GameWindowIdentity selected{reinterpret_cast<HWND>(42), 7, 100, L"C:\\Games\\game.exe"};
  auto current = selected;
  REQUIRE(game_window_identity_equal(selected, current));
  current.process_started_at++;
  REQUIRE_FALSE(game_window_identity_equal(selected, current));
  current = selected; current.process_id++;
  REQUIRE_FALSE(game_window_identity_equal(selected, current));
  current = selected; current.window = reinterpret_cast<HWND>(43);
  REQUIRE_FALSE(game_window_identity_equal(selected, current));
  current = selected; current.executable = L"C:\\Browser\\game.exe";
  REQUIRE_FALSE(game_window_identity_equal(selected, current));
  REQUIRE_FALSE(game_window_identity_equal({}, {}));
}
