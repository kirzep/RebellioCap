#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "core/result.h"
#include "core/qpc_clock.h"
namespace rebelliocap::mf_audio {
using Microsoft::WRL::ComPtr;
struct Failure { HRESULT hr; const char* operation; };
inline void check(HRESULT hr,const char* operation) { if(FAILED(hr)) throw Failure{hr,operation}; }
inline Error error(Failure f) { return {"audio_mf",f.operation,f.hr}; }
struct Runtime {
 bool com=false, mf=false;
 Runtime() {
  const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  if(hr!=RPC_E_CHANGED_MODE) { check(hr,"CoInitializeEx"); com=true; }
  const HRESULT startup=MFStartup(MF_VERSION);
  if(FAILED(startup)) { if(com) CoUninitialize(); throw Failure{startup,"MFStartup"}; } mf=true;
 }
 ~Runtime() { if(mf) MFShutdown(); if(com) CoUninitialize(); }
};
inline std::int64_t scale(std::uint64_t value,std::int64_t frequency,std::uint32_t rate) {
 if(frequency<=0 || !rate || value/rate>static_cast<std::uint64_t>(INT64_MAX/frequency)) throw Failure{E_INVALIDARG,"timestamp overflow"};
 const auto f=static_cast<std::uint64_t>(frequency);
 const auto result=(value/rate)*f+(value%rate)*(f/rate)+(value%rate)*(f%rate)/rate;
 if(result>INT64_MAX) throw Failure{E_INVALIDARG,"timestamp overflow"};
 return static_cast<std::int64_t>(result);
}
inline QpcTicks add_ticks(QpcTicks anchor,QpcTicks offset) {
 if(offset<0 || anchor<0 || anchor>INT64_MAX-offset) throw Failure{E_INVALIDARG,"timestamp addition overflow"};
 return anchor+offset;
}
inline ComPtr<IMFMediaType> pcm_type(std::uint32_t rate,std::uint16_t channels,std::uint32_t mask,bool floating) {
 ComPtr<IMFMediaType> t; check(MFCreateMediaType(&t),"create PCM type");
 check(t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"audio major type");
 check(t->SetGUID(MF_MT_SUBTYPE,floating?MFAudioFormat_Float:MFAudioFormat_PCM),"PCM subtype");
 const UINT32 bytes=floating?4U:2U;
 check(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,channels),"channels"); check(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,rate),"rate");
 check(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,bytes*8),"bits"); check(t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,bytes*channels),"alignment");
 check(t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,rate*bytes*channels),"byte rate");
 check(t->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,mask),"channel mask"); check(t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT,TRUE),"independent PCM");
 return t;
}
inline ComPtr<IMFSample> sample(const void* data,DWORD length,LONGLONG time,LONGLONG duration) {
 ComPtr<IMFSample> s; ComPtr<IMFMediaBuffer> b; check(MFCreateSample(&s),"create sample"); check(MFCreateMemoryBuffer(length,&b),"create buffer");
 BYTE* p=nullptr; check(b->Lock(&p,nullptr,nullptr),"lock input"); std::memcpy(p,data,length); check(b->Unlock(),"unlock input");
 check(b->SetCurrentLength(length),"input length"); check(s->AddBuffer(b.Get()),"attach buffer"); check(s->SetSampleTime(time),"sample time"); check(s->SetSampleDuration(duration),"sample duration"); return s;
}
inline std::vector<ComPtr<IMFSample>> receive(IMFTransform* mft) {
 std::vector<ComPtr<IMFSample>> result;
 for(;;) {
  MFT_OUTPUT_STREAM_INFO info{}; check(mft->GetOutputStreamInfo(0,&info),"output info");
  ComPtr<IMFSample> supplied;
  if(!(info.dwFlags&MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
   ComPtr<IMFMediaBuffer> b; check(MFCreateSample(&supplied),"output sample");
   check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize,65536),info.cbAlignment?info.cbAlignment-1:0,&b),"output buffer"); check(supplied->AddBuffer(b.Get()),"output attach");
  }
  MFT_OUTPUT_DATA_BUFFER out{}; out.pSample=supplied.Get(); DWORD status=0;
  HRESULT hr=mft->ProcessOutput(0,1,&out,&status);
  if(out.pEvents) out.pEvents->Release();
  ComPtr<IMFSample> produced;
  if(out.pSample==supplied.Get()) produced=supplied; else produced.Attach(out.pSample);
  if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT) break;
  check(hr,"ProcessOutput");
  if(!produced) throw Failure{E_UNEXPECTED,"MFT returned no sample"};
  DWORD length=0; check(produced->GetTotalLength(&length),"output length");
  if(length) result.push_back(std::move(produced));
 }
 return result;
}
inline std::vector<std::byte> bytes(IMFSample* s) {
 ComPtr<IMFMediaBuffer> b; check(s->ConvertToContiguousBuffer(&b),"contiguous output"); BYTE* p=nullptr; DWORD length=0;
 check(b->Lock(&p,nullptr,&length),"lock output"); std::vector<std::byte> v(length); std::memcpy(v.data(),p,length); check(b->Unlock(),"unlock output"); return v;
}
inline void start(IMFTransform* mft) { check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0),"begin streaming"); check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0),"start stream"); }
inline std::vector<ComPtr<IMFSample>> feed(IMFTransform* mft,IMFSample* s) {
 std::vector<ComPtr<IMFSample>> result;
 auto hr=mft->ProcessInput(0,s,0);
 if(hr==MF_E_NOTACCEPTING) { result=receive(mft); hr=mft->ProcessInput(0,s,0); }
 check(hr,"ProcessInput"); auto tail=receive(mft); result.insert(result.end(),std::make_move_iterator(tail.begin()),std::make_move_iterator(tail.end())); return result;
}
inline std::vector<ComPtr<IMFSample>> drain(IMFTransform* mft) { check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0),"end stream"); check(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0),"drain"); return receive(mft); }
}
