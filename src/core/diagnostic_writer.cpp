#include "core/diagnostic_writer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace rebelliocap {
namespace {

constexpr std::string_view kRedacted = "[redacted]";

bool is_hexadecimal_pointer(std::string_view value) {
  if (value.size() < 10 || !value.starts_with("0x")) {
    return false;
  }

  return std::ranges::all_of(value.substr(2), [](unsigned char character) {
    return std::isxdigit(character) != 0;
  });
}

bool is_absolute_local_path(std::string_view value) {
  const bool drive_path = value.size() >= 3 &&
                          std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
                          value[1] == ':' && (value[2] == '\\' || value[2] == '/');
  return drive_path || value.starts_with('\\') || value.starts_with('/');
}

bool is_sensitive_string(std::string_view value) {
  return is_hexadecimal_pointer(value) || is_absolute_local_path(value);
}

std::string redact_sensitive_key(std::string_view key) {
  return is_sensitive_string(key) ? std::string(kRedacted) : std::string(key);
}

std::string collision_free_key(const nlohmann::json& object, std::string key) {
  if (!object.contains(key)) {
    return key;
  }

  for (std::size_t suffix = 2;; ++suffix) {
    const auto candidate = key + "#" + std::to_string(suffix);
    if (!object.contains(candidate)) {
      return candidate;
    }
  }
}

nlohmann::json redact_sensitive_values(const nlohmann::json& value) {
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (is_sensitive_string(text)) {
      return std::string(kRedacted);
    }
    return value;
  }

  if (value.is_array()) {
    nlohmann::json sanitized = nlohmann::json::array();
    for (const auto& item : value) {
      sanitized.push_back(redact_sensitive_values(item));
    }
    return sanitized;
  }

  if (value.is_object()) {
    nlohmann::json sanitized = nlohmann::json::object();
    for (const auto& [key, item] : value.items()) {
      sanitized[collision_free_key(sanitized, redact_sensitive_key(key))] =
          redact_sensitive_values(item);
    }
    return sanitized;
  }

  return value;
}

}  // namespace

DiagnosticWriter::DiagnosticWriter(std::ostream& output) : output_(output) {}

void DiagnosticWriter::write(const DiagnosticEvent& event) {
  const nlohmann::json serialized = {
      {"schema", 1},
      {"type", event.type},
      {"qpc", event.qpc},
      {"fields", redact_sensitive_values(event.fields)},
  };
  output_ << serialized.dump() << '\n';
}

}  // namespace rebelliocap
