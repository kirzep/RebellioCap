#pragma once
#include <nlohmann/json.hpp>
#include "hotkey/hotkey_chord.h"
#include "config/recording_contract_json.h"
namespace rebelliocap {
inline const nlohmann::json& recording_contract() {
  static const auto value = [] {
    auto contract = nlohmann::json::parse(kRecordingContractJson);
    if (contract.at("version") != 1) throw std::runtime_error("Unsupported recording contract");
    return contract;
  }();
  return value;
}
inline HotkeyChord recording_default_hotkey(const char* field) {
  const auto& key = recording_contract().at("defaults").at(field);
  return {key.at("key").get<std::uint16_t>(), key.at("ctrl").get<bool>(), key.at("alt").get<bool>(), key.at("shift").get<bool>()};
}
}
