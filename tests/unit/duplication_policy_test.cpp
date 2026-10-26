#include <catch2/catch_test_macros.hpp>
#include "capture/dxgi_capture_source.h"
#include "video/d3d11_nv12_converter.h"
using namespace rebelliocap;

TEST_CASE("direct input fallback preserves terminal errors and submitted ownership", "[direct-nv12]") {
  for (const HRESULT code : {E_INVALIDARG, DXGI_ERROR_UNSUPPORTED}) {
    const Error error{"nv12_converter.input_view_creation_failed", "fixture", static_cast<long>(code)};
    CHECK(can_fallback_direct_input(error, true));
    CHECK_FALSE(can_fallback_direct_input(error, false));
  }
  for (const HRESULT code : {E_OUTOFMEMORY, E_ACCESSDENIED, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_FAIL})
    CHECK_FALSE(can_fallback_direct_input({"nv12_converter.input_view_creation_failed", "fixture", static_cast<long>(code)}, true));
  CHECK_FALSE(can_fallback_direct_input({"nv12_converter.video_process_failed", "fixture", static_cast<long>(E_INVALIDARG)}, true));
  CHECK_FALSE(can_fallback_direct_input({"nv12_converter.input_view_creation_failed", "fixture", std::nullopt}, true));
}

TEST_CASE("duplication selection keeps baseline default without probing modern API", "[duplication-policy]") {
  int modern = 0, legacy = 0;
  const auto result = select_duplication_backend(false,
      [&] { ++modern; return S_OK; }, [&] { ++legacy; return S_OK; });
  REQUIRE(result.is_success()); CHECK(modern == 0); CHECK(legacy == 1);
  CHECK(result.value().backend == CaptureDuplicationBackend::DuplicateOutput);
  CHECK_FALSE(result.value().fallback_hresult.has_value());
}
TEST_CASE("duplication modern selection succeeds without baseline call", "[duplication-policy]") {
  int legacy = 0;
  const auto result = select_duplication_backend(true, [] { return S_OK; }, [&] { ++legacy; return S_OK; });
  REQUIRE(result.is_success()); CHECK(legacy == 0);
  CHECK(result.value().backend == CaptureDuplicationBackend::DuplicateOutput1);
}
TEST_CASE("duplication falls back only for unsupported API interface or format", "[duplication-policy]") {
  for (const HRESULT unsupported : {E_NOINTERFACE, E_NOTIMPL, DXGI_ERROR_UNSUPPORTED}) {
    int modern = 0, legacy = 0;
    const auto result = select_duplication_backend(true,
        [&] { ++modern; return unsupported; }, [&] { ++legacy; return S_OK; });
    REQUIRE(result.is_success()); CHECK(modern == 1); CHECK(legacy == 1);
    CHECK(result.value().backend == CaptureDuplicationBackend::DuplicateOutput);
    REQUIRE(result.value().fallback_hresult.has_value()); CHECK(*result.value().fallback_hresult == unsupported);
  }
}
TEST_CASE("duplication preserves actionable failures without fallback", "[duplication-policy]") {
  for (const HRESULT failure : {E_ACCESSDENIED, DXGI_ERROR_ACCESS_LOST,
       DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, DXGI_ERROR_SESSION_DISCONNECTED,
       DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_INVALIDARG, E_OUTOFMEMORY}) {
    int legacy = 0;
    const auto result = select_duplication_backend(true, [&] { return failure; }, [&] { ++legacy; return S_OK; });
    REQUIRE_FALSE(result.is_success()); CHECK(legacy == 0);
    REQUIRE(result.error().hresult.has_value()); CHECK(*result.error().hresult == static_cast<long>(failure));
  }
}
TEST_CASE("duplication failed baseline fallback reports baseline failure", "[duplication-policy]") {
  const auto result = select_duplication_backend(true, [] { return E_NOINTERFACE; }, [] { return E_ACCESSDENIED; });
  REQUIRE_FALSE(result.is_success()); CHECK(result.error().code == "dxgi_capture.desktop_access_denied");
  CHECK(*result.error().hresult == static_cast<long>(E_ACCESSDENIED));
}
