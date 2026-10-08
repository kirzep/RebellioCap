#include "audio/audio_endpoint_catalog.h"
#include "platform/windows/com_apartment.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>

int main() {
  rebelliocap::ComApartment apartment;
  if (!apartment.initialized()) {
    std::cerr << "audio catalog COM initialization failed: hr=0x" << std::hex
              << static_cast<unsigned long>(apartment.result()) << '\n';
    return 1;
  }

  const auto result = rebelliocap::enumerate_microphones();
  if (!result.is_success()) {
    const auto& error = result.error();
    std::cerr << error.code << ": " << error.message;
    if (error.hresult.has_value()) {
      std::cerr << " hr=0x" << std::hex << static_cast<unsigned long>(*error.hresult);
    }
    std::cerr << '\n';
    return 1;
  }

  const auto& endpoints = result.value();
  if (endpoints.empty()) {
    std::cout << "SKIP: no active capture endpoint is available\n";
    return 77;
  }

  const bool has_invalid_endpoint =
      std::any_of(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
        return endpoint.id.empty() || endpoint.name.empty();
      });
  const auto default_count =
      std::count_if(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
        return endpoint.default_console;
      });

  std::cout << "active_capture_endpoints=" << endpoints.size()
            << " default_console_endpoints=" << default_count << '\n';
  if (has_invalid_endpoint) {
    std::cerr << "An active capture endpoint has an empty ID or friendly name.\n";
    return 1;
  }
  if (default_count != 1) {
    std::cerr << "Expected exactly one default console capture endpoint.\n";
    return 1;
  }
  if (!endpoints.front().default_console) {
    std::cerr << "The default console endpoint is not first in the catalog.\n";
    return 1;
  }

  return 0;
}
