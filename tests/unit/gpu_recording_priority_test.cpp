#include <catch2/catch_test_macros.hpp>
#include "capture/gpu_recording_priority.h"
using namespace rebelliocap;
TEST_CASE("recording priority remains opt in and requires successful driver readback") {
  int calls=0;
  auto disabled=configure_recording_priority(false,[&](int){++calls;return S_OK;},[&](int*){++calls;return S_OK;});
  CHECK(calls==0); CHECK_FALSE(disabled.requested); CHECK_FALSE(disabled.applied);
  auto denied=configure_recording_priority(true,[](int value){CHECK(value==7);return E_ACCESSDENIED;},[](int*){CHECK(false);return S_OK;});
  CHECK(denied.requested); CHECK_FALSE(denied.applied); CHECK(denied.hresult==static_cast<long>(E_ACCESSDENIED));
  auto unreadable=configure_recording_priority(true,[](int){return S_OK;},[](int*){return E_FAIL;});
  CHECK_FALSE(unreadable.applied); CHECK(unreadable.hresult==static_cast<long>(E_FAIL));
  auto ignored=configure_recording_priority(true,[](int){return S_OK;},[](int* value){*value=0;return S_OK;});
  CHECK_FALSE(ignored.applied); CHECK(ignored.effective==0);
  auto applied=configure_recording_priority(true,[](int value){CHECK(value==7);return S_OK;},[](int* value){*value=7;return S_OK;});
  CHECK(applied.applied); CHECK(applied.effective==7); CHECK_FALSE(applied.hresult);
}
