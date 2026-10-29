#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include "capture/d3d11_device.h"
#include "capture/game_window_catalog.h"
#include "capture/wgc_capture_source.h"
#include "core/qpc_clock.h"
using namespace rebelliocap;
using namespace std::chrono_literals;
namespace {
constexpr wchar_t kTitle[] = L"RebellioCap private WGC animated target";
LRESULT CALLBACK target_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_TIMER) { InvalidateRect(window, nullptr, FALSE); return 0; }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT rectangle{}; GetClientRect(window, &rectangle);
    auto brush = CreateSolidBrush(RGB(0, ((GetTickCount64()/80)%2) ? 240 : 180, 0));
    FillRect(dc, &rectangle, brush); DeleteObject(brush); EndPaint(window, &paint); return 0;
  }
  if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}
int target_main() {
  WNDCLASSW cls{}; cls.lpfnWndProc = target_proc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"RebellioCapWgcTarget";
  if (!RegisterClassW(&cls)) return 2;
  HWND window = CreateWindowW(cls.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 440, 340, nullptr, nullptr, cls.hInstance, nullptr);
  if (!window) return 3;
  SetTimer(window, 1, 20, nullptr);
  MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  return 0;
}
void pump() { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
struct Child {
  PROCESS_INFORMATION info{};
  void stop() { if(info.hProcess) { TerminateProcess(info.hProcess, 0); WaitForSingleObject(info.hProcess, 2000); CloseHandle(info.hProcess); CloseHandle(info.hThread); info={}; } }
  ~Child() { stop(); }
};
std::filesystem::path own_path() { std::wstring path(32768,L'\0'); auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size())); path.resize(n); return path; }
std::optional<unsigned> sample(const D3d11Device& device, const CapturedVideoFrame& frame) {
  if (!frame.texture || !frame.lifetime) return std::nullopt;
  Microsoft::WRL::ComPtr<ID3D11Device> owner; frame.texture->GetDevice(&owner);
  if(owner.Get()!=device.device()) return std::nullopt;
  D3D11_TEXTURE2D_DESC desc{}; frame.texture->GetDesc(&desc);
  if(desc.Width!=frame.width || desc.Height!=frame.height || desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) return std::nullopt;
  desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if(FAILED(device.device()->CreateTexture2D(&desc,nullptr,&staging))) return std::nullopt;
  device.context()->CopyResource(staging.Get(),frame.texture.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
  if(FAILED(device.context()->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return std::nullopt;
  auto pixel=static_cast<unsigned char*>(mapped.pData)+(frame.height/2)*mapped.RowPitch+(frame.width/2)*4;
  const bool green=pixel[0]<40 && pixel[1]>=160 && pixel[2]<40;
  unsigned value=pixel[1]; device.context()->Unmap(staging.Get(),0);
  return green ? std::optional<unsigned>{value} : std::nullopt;
}
int fail(const char* reason) { std::cerr<<"FAIL: "<<reason<<'\n'; return 1; }
}
int main(int argc, char** argv) {
  if(argc>1 && std::string(argv[1])=="--target") return target_main();
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory; Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0,&adapter))) { std::cout<<"SKIP: no graphics adapter\n"; return 77; }
  DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc); auto created=create_d3d11_device(desc.AdapterLuid);
  if(!created.is_success()) return fail("D3D11 device creation failed");
  auto device=std::move(created).value(); QpcClock clock;
  auto path=own_path(); Child child; STARTUPINFOW startup{sizeof(startup)};
  auto command=L"\""+path.native()+L"\" --target";
  if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child.info)) return fail("target process creation failed");
  HWND target=nullptr;
  for(int i=0;i<100&&!target;i++) { target=FindWindowW(L"RebellioCapWgcTarget",kTitle); std::this_thread::sleep_for(20ms); }
  if(!target) return fail("target window unavailable");
  auto identity=inspect_game_window(target);
  if(!identity || identity->process_id!=child.info.dwProcessId) return fail("target identity not pinned");
  auto catalog=enumerate_game_windows();
  if(!catalog.is_success()) return fail("window catalog unavailable");
  bool listed=false;
  for(const auto& entry:catalog.value()) {
    if(entry.process_id==GetCurrentProcessId()) return fail("window catalog included the recorder process");
    if(entry.process_id==child.info.dwProcessId && game_executable_equal(entry.executable,path)) listed=true;
  }
  if(!listed) return fail("window catalog omitted the explicit target");
  Child ambiguous_child;
  if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&ambiguous_child.info)) return fail("second target process creation failed");
  // This executable uses the console subsystem; WaitForInputIdle returns
  // ERROR_INVALID_GUI_PROCESS before its window exists. Wait for the exact
  // catalog identity so the ambiguity assertion cannot race initial pinning.
  bool second_ready=false;
  for(int attempt=0;attempt<100&&!second_ready;++attempt) {
    auto windows=enumerate_game_windows();
    if(windows.is_success()) for(const auto& entry:windows.value())
      if(entry.process_id==ambiguous_child.info.dwProcessId && game_executable_equal(entry.executable,path)) second_ready=true;
    if(!second_ready) std::this_thread::sleep_for(20ms);
  }
  if(!second_ready) return fail("second target window unavailable for ambiguity test");
  auto source_result=WgcCaptureSource::create(device,path,clock);
  if(!source_result.is_success()) {
    if(source_result.error().code=="wgc_capture.unsupported") { std::cout<<"SKIP: Windows Graphics Capture unavailable\n"; return 77; }
    std::cerr<<source_result.error().code<<'\n'; return fail("capture source creation failed");
  }
  auto source=std::move(source_result).value(); std::optional<CapturedVideoFrame> retained;
  auto ambiguous=source->next_frame(100ms);
  if(!ambiguous.is_success() || ambiguous.value() || !source->waiting_for_game()) return fail("ambiguous executable silently selected one of two windows");
  ambiguous_child.stop();
  auto collect=[&](int milliseconds, bool require_frame, bool require_animation) {
    std::set<unsigned> colors; int frames=0; auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<deadline) { pump(); auto got=source->next_frame(20ms);
      if(!got.is_success()) { std::cerr<<got.error().code<<'\n'; return false; }
      if(!got.value()) continue;
      auto green=sample(device,*got.value()); if(!green) return false;
      colors.insert(*green); ++frames; retained=std::move(got.value());
    }
    std::cout<<"frames="<<frames<<" green_levels="<<colors.size()<<'\n';
    return (!require_frame || frames>0) && (!require_animation || colors.size()>1);
  };
  if(!collect(2200,true,true)) return fail("initial target frames missing/incorrect");
  auto leased_first=std::move(retained); retained.reset();
  auto leased_second=source->next_frame(500ms);
  if(!leased_second.is_success() || !leased_second.value() || !sample(device,*leased_second.value())) return fail("second owned lease unavailable");
  auto saturated=source->next_frame(100ms);
  if(!saturated.is_success() || saturated.value()) return fail("owned texture pool grew beyond two outstanding leases");
  leased_first.reset(); leased_second.value().reset();
  HWND cover=CreateWindowExW(WS_EX_TOPMOST,L"STATIC",L"Private magenta cover",WS_POPUP|WS_VISIBLE,60,60,500,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  auto magenta=CreateSolidBrush(RGB(255,0,255)); SetClassLongPtrW(cover,GCLP_HBRBACKGROUND,reinterpret_cast<LONG_PTR>(magenta)); InvalidateRect(cover,nullptr,TRUE); UpdateWindow(cover); SetForegroundWindow(cover);
  if(!collect(1200,true,true)) return fail("occluded target switched pixels or stopped animating");
  ShowWindow(target,SW_MINIMIZE);
  if(!collect(500,false,false)) return fail("minimize leaked unrelated pixels");
  if(!source->waiting_for_game()) return fail("minimized target did not report waiting");
  if(!inspect_game_window(target)) return fail("minimized explicit target lost its selectable identity");
  ShowWindow(target,SW_RESTORE);
  if(!collect(1600,true,true)) return fail("restore did not resume target");
  SetWindowPos(target,nullptr,80,80,300,240,SWP_NOZORDER|SWP_NOACTIVATE);
  if(!collect(700,true,false)) return fail("shrink returned uncropped/incorrect pixels");
  SetWindowPos(target,nullptr,80,80,520,420,SWP_NOZORDER|SWP_NOACTIVATE);
  if(!collect(700,true,false)) return fail("growth returned clipped/incorrect pixels");
  PostMessageW(target,WM_CLOSE,0,0); WaitForSingleObject(child.info.hProcess,2000);
  auto closed=source->next_frame(100ms);
  if(!closed.is_success() || closed.value() || !source->waiting_for_game()) return fail("closed source did not wait without pixels");
  if(inspect_game_window(target)) return fail("closed target retained a live catalog identity");
  child.stop();
  if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child.info)) return fail("reopened target process creation failed");
  WaitForInputIdle(child.info.hProcess,2000);
  if(!collect(1800,true,true)) return fail("reopened selected executable did not recover");
  source.reset(); if(!retained || !sample(device,*retained)) return fail("retained texture did not survive source destruction");
  DestroyWindow(cover); DeleteObject(magenta);
  std::cout<<"PASS: selected-window privacy: catalog, ambiguity, two-slot lease bound, cover/focus, minimize, restore, shrink/grow, close/reopen, retained texture\n";
  return 0;
}
