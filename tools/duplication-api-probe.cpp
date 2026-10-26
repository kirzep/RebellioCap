// Read-only developer capability probe. Does not change display modes or
// capture/save pixels. Records native API HRESULTs separately from app guards.
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include "capture/monitor_catalog.h"
using Microsoft::WRL::ComPtr;
using json=nlohmann::json;
using namespace rebelliocap;
int main(){
  auto monitors=enumerate_monitors();
  if(!monitors.is_success())return 1;
  const auto found=std::find_if(monitors.value().begin(),monitors.value().end(),[](const auto& m){return m.primary;});
  if(found==monitors.value().end())return 77;
  ComPtr<IDXGIFactory1> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 1;
  ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;;++i){
    ComPtr<IDXGIAdapter1> candidate;
    const auto hr=factory->EnumAdapters1(i,&candidate);
    if(FAILED(hr))return 1;
    DXGI_ADAPTER_DESC1 d{};if(FAILED(candidate->GetDesc1(&d)))return 1;
    if(d.AdapterLuid.LowPart==found->id.adapter_luid.LowPart&&d.AdapterLuid.HighPart==found->id.adapter_luid.HighPart){adapter=std::move(candidate);break;}
  }
  ComPtr<IDXGIOutput> output;if(FAILED(adapter->EnumOutputs(found->id.output_index,&output)))return 1;
  json report={{"scope","read-only native duplication and video-interface capability probe; no display changes, saved pixels or performance claim"},{"rows",json::array()}};
  for(const auto level:{D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1})for(const bool video_flag:{false,true}){
    const UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT|(video_flag?D3D11_CREATE_DEVICE_VIDEO_SUPPORT:0U);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL accepted{};
    const auto created=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,&level,1,D3D11_SDK_VERSION,&device,&accepted,&context);
    json row={{"requested_feature_level",static_cast<UINT>(level)},{"video_support_flag",video_flag},{"device_hresult",static_cast<long>(created)}};
    if(SUCCEEDED(created)){
      row["accepted_feature_level"]=static_cast<UINT>(accepted);
      ComPtr<ID3D11VideoDevice> vd;ComPtr<ID3D11VideoContext> vc;
      row["video_device_hresult"]=static_cast<long>(device.As(&vd));
      row["video_context_hresult"]=static_cast<long>(context.As(&vc));
      if(vd){
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate=content.OutputFrameRate={60,1};
        content.InputWidth=1920;content.InputHeight=1080;content.OutputWidth=1280;content.OutputHeight=720;
        content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        const auto enumerated=vd->CreateVideoProcessorEnumerator(&content,&enumerator);
        row["processor_enumerator_hresult"]=static_cast<long>(enumerated);
        if(SUCCEEDED(enumerated)){
          ComPtr<ID3D11VideoProcessor> processor;
          row["processor_hresult"]=static_cast<long>(vd->CreateVideoProcessor(enumerator.Get(),0,&processor));
        }
      }
      ComPtr<IDXGIOutput5> modern;
      const auto queried=output.As(&modern);row["output5_hresult"]=static_cast<long>(queried);
      ComPtr<IDXGIOutputDuplication> duplication;
      if(SUCCEEDED(queried)){
        constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_B8G8R8A8_UNORM};
        row["native_duplicate_output1_hresult"]=static_cast<long>(modern->DuplicateOutput1(device.Get(),0,1,formats,&duplication));
      }
      if(!duplication){
        ComPtr<IDXGIOutput1> legacy;
        if(SUCCEEDED(output.As(&legacy)))row["legacy_hresult"]=static_cast<long>(legacy->DuplicateOutput(device.Get(),&duplication));
      }
      if(duplication){
        DXGI_OUTDUPL_DESC d{};duplication->GetDesc(&d);row["description_format"]=static_cast<UINT>(d.ModeDesc.Format);
        DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
        const auto acquired=duplication->AcquireNextFrame(1000,&info,&resource);
        row["acquire_hresult"]=static_cast<long>(acquired);
        if(SUCCEEDED(acquired)){
          ComPtr<ID3D11Texture2D> texture;
          if(SUCCEEDED(resource.As(&texture))){D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);row["texture_format"]=static_cast<UINT>(td.Format);}
          row["release_hresult"]=static_cast<long>(duplication->ReleaseFrame());
        }
      }
    }
    report["rows"].push_back(std::move(row));
  }
  std::cout<<report.dump(2)<<'\n';return 0;
}
