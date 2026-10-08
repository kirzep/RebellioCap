#include <catch2/catch_test_macros.hpp>
#include "engine/recording_name.h"
#include <fstream>
#include <chrono>
using namespace rebelliocap;
TEST_CASE("ShadowPlay format captures game and centiseconds"){
 NameContext c;c.game=L"Cyberpunk 2077";c.time={2023,3,1,27,13,30,59,620};
 const auto parts=nlohmann::json::array({{{"kind","token"},{"value","game"}},{{"kind","text"},{"value"," "}},{{"kind","token"},{"value","date"}},{{"kind","text"},{"value"," - "}},{{"kind","token"},{"value","time_extended"}},{{"kind","text"},{"value",".DVR"}}});
 REQUIRE(format_recording_name(parts,c)==L"Cyberpunk 2077 2023.03.27 - 13.30.59.62.DVR");
}
TEST_CASE("Custom name text cannot escape the output folder"){
 NameContext c;c.counter=42;
 REQUIRE(format_recording_name(nlohmann::json::array({{{"kind","text"},{"value","../CON: / test* "}},{{"kind","token"},{"value","counter"}}}),c)==L".._CON_ _ test_ 42");
 REQUIRE(format_recording_name(nlohmann::json::array({{{"kind","text"},{"value","CON"}}}),c)==L"_CON");
 REQUIRE_THROWS(format_recording_name(nlohmann::json::array({{{"kind","token"},{"value","arbitrary"}}}),c));
}
TEST_CASE("Name presets update without a recorder restart and counter persists"){
 const auto dir=std::filesystem::temp_directory_path()/("rebcap-names-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directory(dir);
 struct Cleanup{std::filesystem::path dir;~Cleanup(){std::error_code ec;std::filesystem::remove_all(dir,ec);}} cleanup{dir};
 const auto path=dir/"recording-names.json";
 auto write=[&](const std::string& prefix){std::ofstream file(path);file<<nlohmann::json{{"activeId","user"},{"presets",nlohmann::json::array({{{"id","user"},{"parts",nlohmann::json::array({{{"kind","text"},{"value",prefix}},{{"kind","token"},{"value","counter"}}})}}})}};};
 write("One ");RecordingNames names(path);NameContext c;
 REQUIRE(names.capture(c)==L"One 1");
 write("Two ");names.reload();REQUIRE(names.capture(c)==L"Two 2");names.persist_counter();
 RecordingNames restarted(path);REQUIRE(restarted.capture(c)==L"Two 3");
}
TEST_CASE("Capturing a name uses cached settings and does not persist on the caller") {
 const auto dir=std::filesystem::temp_directory_path()/("rebcap-names-cache-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directory(dir);
 struct Cleanup{std::filesystem::path dir;~Cleanup(){std::error_code ec;std::filesystem::remove_all(dir,ec);}} cleanup{dir};
 const auto path=dir/"recording-names.json";
 auto write=[&](const char* prefix){std::ofstream file(path);file<<nlohmann::json{{"activeId","user"},{"presets",nlohmann::json::array({{{"id","user"},{"parts",nlohmann::json::array({{{"kind","text"},{"value",prefix}},{{"kind","token"},{"value","counter"}}})}}})}};};
 write("Cached ");
 RecordingNames names(path);
 REQUIRE(names.capture(NameContext{})==L"Cached 1");
 REQUIRE_FALSE(std::filesystem::exists(dir/"recording-counter.txt"));
 write("Changed ");
 REQUIRE(names.capture(NameContext{})==L"Cached 2");
 names.reload();
 REQUIRE(names.capture(NameContext{})==L"Changed 3");
 names.persist_counter();
 RecordingNames restarted(path);
 REQUIRE(restarted.capture(NameContext{})==L"Changed 4");
 {std::ofstream invalid(path);invalid<<"{broken";}
 restarted.reload();
 REQUIRE(restarted.capture(NameContext{}).starts_with(L"Desktop "));
 REQUIRE(restarted.capture(NameContext{}).ends_with(L".DVR"));
}
