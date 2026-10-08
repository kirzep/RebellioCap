#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

std::string utf8(const std::wstring& value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::filesystem::path executable_directory() {
  std::wstring path(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                          static_cast<DWORD>(path.size()));
  path.resize(length);
  return std::filesystem::path(path).parent_path().parent_path();
}

int validate_clip(const std::filesystem::path& path) {
  AVFormatContext* context = nullptr;
  const auto narrow = utf8(path.wstring());
  if (avformat_open_input(&context, narrow.c_str(), nullptr, nullptr) < 0) return 1;
  struct CloseInput {
    AVFormatContext** context;
    ~CloseInput() { avformat_close_input(context); }
  } close{&context};
  if (avformat_find_stream_info(context, nullptr) < 0) return 1;
  unsigned int video = 0;
  unsigned int audio = 0;
  for (unsigned int index = 0; index < context->nb_streams; ++index) {
    if (context->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ++video;
    if (context->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++audio;
  }
  if (video != 1 || audio != 3 || context->duration < 20 * AV_TIME_BASE) {
    std::cerr << "unexpected smoke streams video=" << video << " audio=" << audio
              << " duration=" << context->duration << '\n';
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  const auto engine = executable_directory() / L"RebellioCap.Engine.exe";
  wchar_t temporary_directory[MAX_PATH]{};
  if (GetTempPathW(MAX_PATH, temporary_directory) == 0) return 1;
  const auto output = std::filesystem::path(temporary_directory) /
      (L"rebelliocap-pipeline-" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
  DeleteFileW(output.c_str());

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return 1;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::wstring command = L"\"" + engine.wstring() +
      L"\" smoke --seconds 30 --container mp4 --output \"" + output.wstring() + L"\"";
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL created = CreateProcessW(engine.c_str(), mutable_command.data(), nullptr,
                                      nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                      executable_directory().c_str(), &startup, &process);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    return 1;
  }

  std::string diagnostics;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(55);
  DWORD exit_code = STILL_ACTIVE;
  while (std::chrono::steady_clock::now() < deadline) {
    DWORD available = 0;
    if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (ReadFile(read_pipe, chunk.data(), available, &read, nullptr)) {
        chunk.resize(read);
        diagnostics += chunk;
      }
    }
    if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) break;
  }
  if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5'000);
  }
  while (true) {
    char buffer[4096];
    DWORD read = 0;
    if (!ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
    diagnostics.append(buffer, read);
  }
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_pipe);

  if (diagnostics.find("dxgi_capture.desktop_access_denied") != std::string::npos ||
      exit_code == 3) {
    std::cout << "not-run: interactive capture hardware is unavailable\n";
    DeleteFileW(output.c_str());
    return 77;
  }
  if (exit_code != 0) {
    std::cerr << diagnostics;
    DeleteFileW(output.c_str());
    return 1;
  }
  const int validation = validate_clip(output);
  DeleteFileW(output.c_str());
  return validation;
}
