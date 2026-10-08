#include "engine/game_category.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rebelliocap {
namespace {
#pragma pack(push, 1)
struct IconHeader { WORD reserved, type, count; };
struct GroupEntry {
  BYTE width, height, colors, reserved;
  WORD planes, bits;
  DWORD bytes;
  WORD resource;
};
struct FileEntry {
  BYTE width, height, colors, reserved;
  WORD planes, bits;
  DWORD bytes, offset;
};
#pragma pack(pop)
static_assert(sizeof(GroupEntry) == 14 && sizeof(FileEntry) == 16);

struct ResourceChoice { HRSRC group{}; };
BOOL CALLBACK first_group(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context) {
  auto& choice = *reinterpret_cast<ResourceChoice*>(context);
  choice.group = FindResourceW(module, name, type);
  return FALSE;
}
struct ModuleGuard { HMODULE value; ~ModuleGuard() { FreeLibrary(value); } };
struct FileGuard { HANDLE value; ~FileGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
}

bool cache_game_icon(const std::filesystem::path& executable,
                     const std::filesystem::path& directory) noexcept {
  try {
    const auto directory_attributes = GetFileAttributesW(directory.c_str());
    if (directory_attributes == INVALID_FILE_ATTRIBUTES ||
        !(directory_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    const auto target = directory / L".game-icon.ico";
    const auto existing = GetFileAttributesW(target.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES)
      return !(existing & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    if (!executable.is_absolute()) return false;
    // Resource-only mapping: no imports, DllMain, or game code are executed.
    const auto module = LoadLibraryExW(executable.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return false;
    ModuleGuard module_guard{module};
    ResourceChoice choice;
    EnumResourceNamesW(module, MAKEINTRESOURCEW(14), first_group, reinterpret_cast<LONG_PTR>(&choice));
    if (!choice.group) return false;
    const auto group_size = SizeofResource(module, choice.group);
    const auto group = static_cast<const BYTE*>(LockResource(LoadResource(module, choice.group)));
    if (!group || group_size < sizeof(IconHeader)) return false;
    IconHeader header{};
    std::memcpy(&header, group, sizeof(header));
    if (header.reserved || header.type != 1 || !header.count || header.count > 256 ||
        group_size < sizeof(header) + header.count * sizeof(GroupEntry)) return false;
    GroupEntry best{};
    unsigned best_score = 0;
    for (unsigned i = 0; i < header.count; ++i) {
      GroupEntry entry{};
      std::memcpy(&entry, group + sizeof(header) + i * sizeof(entry), sizeof(entry));
      const auto width = entry.width ? entry.width : 256u;
      const auto height = entry.height ? entry.height : 256u;
      if (!entry.bytes || entry.bytes > 2 * 1024 * 1024) continue;
      const auto score = width * height * 64u + (std::min)(entry.bits, WORD{32});
      if (score > best_score) { best = entry; best_score = score; }
    }
    if (!best_score) return false;
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(best.resource), MAKEINTRESOURCEW(3));
    if (!resource || SizeofResource(module, resource) != best.bytes) return false;
    const auto data = static_cast<const BYTE*>(LockResource(LoadResource(module, resource)));
    if (!data) return false;
    const IconHeader file_header{0, 1, 1};
    const FileEntry entry{best.width, best.height, best.colors, 0, best.planes,
        best.bits, best.bytes, sizeof(IconHeader) + sizeof(FileEntry)};
    std::vector<BYTE> bytes(entry.offset + entry.bytes);
    std::memcpy(bytes.data(), &file_header, sizeof(file_header));
    std::memcpy(bytes.data() + sizeof(file_header), &entry, sizeof(entry));
    std::memcpy(bytes.data() + entry.offset, data, entry.bytes);
    const auto temporary = directory / (L".game-icon-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetCurrentThreadId()) + L".tmp");
    FileGuard file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size();
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    const bool published = success && MoveFileExW(temporary.c_str(), target.c_str(), 0);
    if (!published) DeleteFileW(temporary.c_str());
    return published;
  } catch (...) { return false; }
}
}  // namespace rebelliocap
