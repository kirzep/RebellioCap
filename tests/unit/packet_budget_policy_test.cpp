#include <catch2/catch_test_macros.hpp>
#include "engine/packet_budget_policy.h"
using namespace rebelliocap;
namespace { constexpr std::uint64_t MiB=1024ULL*1024; }
TEST_CASE("automatic packet RAM budgets preserve the current conservative policy") {
  REQUIRE(packet_budget_policy(0,PhysicalMemory{16*1024*MiB,8*1024*MiB}).value()==1024*MiB);
  REQUIRE(packet_budget_policy(0,PhysicalMemory{16*1024*MiB,512*MiB}).value()==64*MiB);
  REQUIRE(packet_budget_policy(0,PhysicalMemory{1024*MiB,1024*MiB}).value()==64*MiB);
  REQUIRE(packet_budget_policy(0,std::nullopt).value()==64*MiB);
  REQUIRE(packet_budget_policy(0,PhysicalMemory{1024*MiB,0}).value()==0);
}
TEST_CASE("manual packet RAM budgets are exact and respect available and total RAM") {
  const PhysicalMemory ample{16*1024*MiB,8*1024*MiB};
  REQUIRE(packet_budget_policy(2048,ample).value()==2048*MiB);
  REQUIRE(packet_budget_policy(4096,ample).value()==4096*MiB);
  const auto insufficient=packet_budget_policy(4097,ample);
  REQUIRE_FALSE(insufficient.is_success());
  REQUIRE(insufficient.error().code=="config.replay_memory_unavailable");
  REQUIRE(insufficient.error().message.find("4096")!=std::string::npos);
  REQUIRE(insufficient.error().message.find("8192")!=std::string::npos);
  REQUIRE_FALSE(packet_budget_policy(1025,PhysicalMemory{16*1024*MiB,2048*MiB}).is_success());
  REQUIRE_FALSE(packet_budget_policy(513,PhysicalMemory{2048*MiB,2048*MiB}).is_success());
  REQUIRE_FALSE(packet_budget_policy(64,PhysicalMemory{1024*MiB,0}).is_success());
  REQUIRE_FALSE(packet_budget_policy(64,std::nullopt).is_success());
  REQUIRE_FALSE(packet_budget_policy(64,PhysicalMemory{0,0}).is_success());
}
TEST_CASE("manual packet RAM validation follows the recording contract") {
  for(const auto invalid:{1U,63U,8193U,0xffffffffU}) {
    const auto result=packet_budget_policy(invalid,PhysicalMemory{128*1024*MiB,128*1024*MiB});
    REQUIRE_FALSE(result.is_success());
    REQUIRE(result.error().code=="config.invalid");
  }
  REQUIRE(packet_budget_policy(8192,PhysicalMemory{128*1024*MiB,128*1024*MiB}).value()==8192*MiB);
}
