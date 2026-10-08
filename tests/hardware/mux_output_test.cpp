#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string_view>
#include <Windows.h>
#include <objbase.h>
#include "mux/ffmpeg_clip_muxer.h"
#include "mux/ffmpeg_continuous_muxer.h"
using namespace rebelliocap;
TEST_CASE("mux refuses an existing destination without touching it") {
 GUID id{}; REQUIRE(SUCCEEDED(CoCreateGuid(&id))); wchar_t name[40]{}; StringFromGUID2(id,name,40);
 const auto directory = std::filesystem::temp_directory_path() / (std::wstring(L"RebellioCap-mux-")+name);
 REQUIRE(std::filesystem::create_directory(directory));
 const auto final = directory / L"existing-юникод.mp4";
 { std::ofstream f(final); f << "sentinel"; }
 FfmpegClipMuxer mux;
 auto result = mux.write({}, {}, final, Container::Mp4);
 REQUIRE_FALSE(result.is_success());
 CHECK(result.error().code == "mux.destination_exists");
 std::ifstream f(final); std::string contents; f >> contents;
 CHECK(contents == "sentinel"); f.close(); std::filesystem::remove(final); std::filesystem::remove(directory);
}
#include <Windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <cmath>
#include "video/nvenc_encoder.h"
#include "video/d3d11_nv12_converter.h"
namespace {
using Microsoft::WRL::ComPtr;
using nlohmann::json;
struct Fixture { ReplaySnapshot snapshot; std::vector<StreamDescriptor> descriptors; std::filesystem::path artifacts; };
Fixture generate_fixture(bool include_audio=true) {
 Fixture f; QpcClock clock; const auto step=clock.frequency()/60; const auto origin=clock.frequency()*10;
 GUID guid{}; REQUIRE(SUCCEEDED(CoCreateGuid(&guid))); wchar_t name[40]{}; StringFromGUID2(guid,name,40);
 f.artifacts=std::filesystem::path(REBELLIOCAP_SOURCE_DIR)/"artifacts"/(std::wstring(L"task-10-")+name);
 REQUIRE(std::filesystem::create_directories(f.artifacts));
 ComPtr<IDXGIFactory1> factory; REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i) { ComPtr<IDXGIAdapter1> candidate; REQUIRE(factory->EnumAdapters1(i,&candidate)!=DXGI_ERROR_NOT_FOUND); DXGI_ADAPTER_DESC1 d{}; REQUIRE(SUCCEEDED(candidate->GetDesc1(&d)));if(d.VendorId==0x10de){adapter=std::move(candidate);break;} }
 ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
 REQUIRE(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
 constexpr UINT width=640,height=360;
 D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
 CapturedVideoFrame frame{};frame.width=width;frame.height=height;frame.captured_at=origin;frame.lifetime=std::make_shared<int>(1);
 REQUIRE(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&frame.texture)));
 ComPtr<ID3D11RenderTargetView> target; REQUIRE(SUCCEEDED(device->CreateRenderTargetView(frame.texture.Get(),nullptr,&target)));
 auto converted=D3d11Nv12Converter::create(device.Get(),context.Get(),{width,height,width,height,8});REQUIRE(converted.is_success());auto converter=std::move(converted).value();
 auto created=NvencEncoder::create(device.Get(),{width,height,60,3000000,2},clock); if(!created.is_success()) INFO(created.error().code<<": "<<created.error().message); REQUIRE(created.is_success());auto encoder=std::move(created).value();
 f.descriptors.push_back(encoder->descriptor()); REQUIRE(f.descriptors[0].width==width);REQUIRE(f.descriptors[0].height==height);REQUIRE_FALSE(f.descriptors[0].codec_extradata.empty());
 std::vector<EncodedPacket> video;
 for(int i=0;i<90;++i) {
  const float color[]{static_cast<float>(i)/100.0f,0.2f,0.4f,1};context->ClearRenderTargetView(target.Get(),color);
  auto nv12=converter->convert(frame);REQUIRE(nv12.is_success());auto encoded=encoder->encode(std::move(nv12).value(),origin+step*i,false); REQUIRE(encoded.is_success());
  for(auto& p:encoded.value()) video.push_back(std::move(p));
 }
 auto flush=encoder->flush();REQUIRE(flush.is_success());for(auto& p:flush.value())video.push_back(std::move(p));REQUIRE(video.size()==90); REQUIRE(encoder->metrics().b_picture_frames>0);
 // Full raw output is retained for provenance and independent decode comparison.
 json provenance={{"generator","Direct NVENC, GPU BGRA pattern -> NV12; no captured user content"},{"qpc_frequency",clock.frequency()},{"width",width},{"height",height},{"frame_duration",step},{"origin",origin},{"packets",json::array()}};
 std::ofstream raw(f.artifacts/"video.h264",std::ios::binary); std::size_t offset=0;
 for(const auto& p:video) {raw.write(reinterpret_cast<const char*>(p.payload->data()),static_cast<std::streamsize>(p.payload->size()));provenance["packets"].push_back({{"offset",offset},{"size",p.payload->size()},{"pts",p.pts},{"dts",p.dts},{"duration",p.duration},{"keyframe",p.keyframe}});offset+=p.payload->size();} raw.close();
 ReplayRing ring(clock.frequency()*30);for(const auto& p:video)REQUIRE(ring.append(p).is_success());
 bool selected=false;
 for(int i=40;i<60;++i) {auto cut=ring.snapshot(origin+step*i,step*i);REQUIRE(cut.is_success());if(std::any_of(cut.value().packets.begin(),cut.value().packets.end(),[&](const auto& p){return p.pts>cut.value().end;})){f.snapshot=std::move(cut).value();selected=true;break;}}
 REQUIRE(selected);provenance["cut_end"]=f.snapshot.end;provenance["cut_video_packets"]=f.snapshot.packets.size();
 const auto fixture_dir=std::filesystem::path(REBELLIOCAP_SOURCE_DIR)/"tests/data/mux-aac";
 for(int stream=0;include_audio && stream<3;++stream) {
  std::ifstream index(fixture_dir/("aac-"+std::to_string(stream)+".json"));json j;index>>j;REQUIRE(j["qpc_frequency"]==clock.frequency());
  StreamDescriptor d{static_cast<StreamKind>(stream+1),"aac",stream==0?"Mixed":stream==1?"System":"Microphone",{}, {1,static_cast<std::int32_t>(clock.frequency())},0,0,j["sample_rate"],j["channels"]};for(int b:j["asc"])d.codec_extradata.push_back(static_cast<std::byte>(b));f.descriptors.push_back(d);
  std::ifstream payload(fixture_dir/("aac-"+std::to_string(stream)+".bin"),std::ios::binary);
  const auto original=j["packets"][0]["pts"].get<QpcTicks>();
  for(const auto& p:j["packets"]) {const auto pts=p["pts"].get<QpcTicks>()-original+origin;if(pts>f.snapshot.end)break; auto bytes=std::make_shared<std::vector<std::byte>>(p["size"].get<std::size_t>());payload.seekg(p["offset"].get<std::streamoff>());payload.read(reinterpret_cast<char*>(bytes->data()),static_cast<std::streamsize>(bytes->size()));REQUIRE(payload.good()); f.snapshot.packets.push_back({d.kind,0,pts,pts,p["duration"],true,bytes});}
 }
 std::stable_sort(f.snapshot.packets.begin(),f.snapshot.packets.end(),[](const auto& a,const auto& b){return a.dts<b.dts;});
 provenance["audio_origin_policy"]="Each saved MF AAC stream rebased independently onto synthetic video origin. Not live A/V synchronization evidence.";
 std::ofstream manifest(f.artifacts/"fixture-provenance.json");manifest<<provenance.dump(2);
 std::cout<<"MUX_ARTIFACTS="<<f.artifacts.string()<<'\n';return f;
}
json probe(const std::filesystem::path& file,const std::filesystem::path& artifacts) {
 const auto out=artifacts/(file.filename().wstring()+L".probe.json"),err=artifacts/(file.filename().wstring()+L".probe.stderr");
 SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE stdout_file=CreateFileW(out.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,0,nullptr); REQUIRE(stdout_file!=INVALID_HANDLE_VALUE);
 HANDLE stderr_file=CreateFileW(err.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,0,nullptr);REQUIRE(stderr_file!=INVALID_HANDLE_VALUE);
 const auto executable=std::filesystem::path(REBELLIOCAP_SOURCE_DIR)/".tools/vcpkg_installed/x64-windows/tools/ffmpeg/ffprobe.exe";
 std::wstring cmd=L"\""+executable.wstring()+L"\" -v repeat+warning -err_detect explode -show_streams -show_format -show_packets -show_frames -of json \""+file.wstring()+L"\"";
 STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=stdout_file;startup.hStdError=stderr_file;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION process{};
 REQUIRE(CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process));
 REQUIRE(WaitForSingleObject(process.hProcess,60000)==WAIT_OBJECT_0);DWORD code=1;REQUIRE(GetExitCodeProcess(process.hProcess,&code));CloseHandle(process.hProcess);CloseHandle(process.hThread);CloseHandle(stdout_file);CloseHandle(stderr_file);REQUIRE(code==0);
 std::ifstream diagnostics(err); std::string line;
 while(std::getline(diagnostics,line)) {
  INFO(line);
  CHECK(line.find("UDTA parsing failed")==std::string::npos);
  // Fragmented recovery lacks a trailer; H.264 discovers its B-frame depth
  // while decoding. Keep all other warnings/errors fatal in this fixture.
  REQUIRE(line.find("Increasing reorder buffer to 2")!=std::string::npos);
 }
 std::ifstream data(out);json j;data>>j;return j;
}
}
namespace rebelliocap {
struct FfmpegMuxTestAccess {
 static void observe(FfmpegClipMuxer& mux,std::function<void(const std::filesystem::path&,bool)> hook) { mux.lifecycle_observer_=std::move(hook); }
};
}
TEST_CASE("real reordered NVENC and saved MF AAC mux to MP4 and MKV") {
 auto fixture=generate_fixture();FfmpegClipMuxer mux;
 for(auto container:{Container::Mp4,Container::Mkv}) {
  const auto final=fixture.artifacts/(container==Container::Mp4?L"клип.mp4":L"клип.mkv");
  auto result=mux.write(fixture.snapshot,fixture.descriptors,final,container);if(!result.is_success()) INFO(result.error().code<<": "<<result.error().message);REQUIRE(result.is_success()); REQUIRE(std::filesystem::exists(final));
  auto j=probe(final,fixture.artifacts);REQUIRE(j["streams"].size()==4);CHECK(j["streams"][0]["codec_name"]=="h264");CHECK(j["streams"][0]["width"]==640);CHECK(j["streams"][0]["height"]==360);
  for(int i=0;i<4;++i) {const auto& tags=j["streams"][i]["tags"];const auto title=tags.contains("title")?tags["title"]:tags["handler_name"];CHECK(title==stream_title(fixture.descriptors[static_cast<std::size_t>(i)].kind));}
  for(int i=1;i<4;++i) {CHECK(j["streams"][i]["codec_name"]=="aac");CHECK(j["streams"][i]["sample_rate"]=="48000");CHECK(j["streams"][i]["channels"]==2);}
  const auto& tags=j["format"]["tags"];const char* key=container==Container::Mp4?"rebelliocap_requested_end_qpc":"REBELLIOCAP_REQUESTED_END_QPC";CHECK(tags[key]==std::to_string(fixture.snapshot.end-fixture.snapshot.actual_start));
  const double tolerance=container==Container::Mp4?0.00002:0.0011;
  std::vector<double> expected_video_pts;
  QpcTicks end=0,video_end=0,tail=0;
  std::array<std::size_t,4> expected_counts{},actual_counts{};
  for(const auto& p:fixture.snapshot.packets) {
   ++expected_counts[static_cast<std::size_t>(p.stream)];
   end=(std::max)(end,p.pts+p.duration-fixture.snapshot.actual_start);
   if(p.stream==StreamKind::Video) {expected_video_pts.push_back(static_cast<double>(p.pts-fixture.snapshot.actual_start)/QpcClock{}.frequency());video_end=(std::max)(video_end,p.pts+p.duration-fixture.snapshot.actual_start);tail=(std::max)(tail,p.pts-fixture.snapshot.end);}
  }
  std::sort(expected_video_pts.begin(),expected_video_pts.end());
  const auto tag=[&](std::string name) {if(container==Container::Mkv)for(char& c:name)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return tags[name].get<std::string>();};
  CHECK(tag("rebelliocap_requested_start_qpc")==std::to_string(fixture.snapshot.requested_start-fixture.snapshot.actual_start));
  CHECK(tag("rebelliocap_actual_start_qpc")=="0");CHECK(tag("rebelliocap_actual_end_qpc")==std::to_string(end));CHECK(tag("rebelliocap_video_end_qpc")==std::to_string(video_end));CHECK(tag("rebelliocap_dependency_tail_qpc")==std::to_string(tail));CHECK(tag("rebelliocap_qpc_frequency")==std::to_string(QpcClock{}.frequency()));
  std::array<double,4> last_dts{};last_dts.fill(-100.0);
  for(const auto& entry:j["packets_and_frames"])if(entry["type"]=="packet") {
   const auto i=entry["stream_index"].get<std::size_t>();REQUIRE(i<4);++actual_counts[i];
   if(entry.contains("dts_time")) {const auto dts=std::stod(entry["dts_time"].get<std::string>());CHECK(dts>last_dts[i]);last_dts[i]=dts;}
  }
  CHECK(actual_counts==expected_counts);
  CHECK(std::abs(std::stod(j["format"]["duration"].get<std::string>())-static_cast<double>(end)/QpcClock{}.frequency())<0.002);
  std::size_t decoded=0,bframes=0;double previous=-1;
  for(const auto& entry:j["packets_and_frames"]) if(entry["type"]=="frame" && entry["media_type"]=="video") {const auto pts=std::stod(entry["best_effort_timestamp_time"].get<std::string>());CHECK(pts>=previous);REQUIRE(decoded<expected_video_pts.size());CHECK(std::abs(pts-expected_video_pts[decoded])<=tolerance);previous=pts;++decoded;if(entry["pict_type"]=="B")++bframes;}
  const auto expected=std::count_if(fixture.snapshot.packets.begin(),fixture.snapshot.packets.end(),[](const auto& p){return p.stream==StreamKind::Video;});CHECK(decoded==static_cast<std::size_t>(expected));CHECK(bframes>0);
  CHECK(std::abs(std::stod(j["format"]["start_time"].get<std::string>()))<0.002);
  CHECK(previous>static_cast<double>(fixture.snapshot.end-fixture.snapshot.actual_start)/QpcClock{}.frequency());
  auto duplicate=mux.write(fixture.snapshot,fixture.descriptors,final,container);CHECK_FALSE(duplicate.is_success());CHECK(duplicate.error().code=="mux.destination_exists");
 }
}

TEST_CASE("continuous recovery preserves reordered NVENC and all AAC tracks") {
 auto fixture=generate_fixture();
 fixture.descriptors[0].title="Game — тест";
 for(auto container:{Container::Mp4,Container::Mkv}) {
  FfmpegContinuousMuxer mux;
  const auto destination=fixture.artifacts/(container==Container::Mp4?L"recovery.mp4":L"recovery.mkv");
  REQUIRE(mux.open(fixture.descriptors,destination,container).is_success());
  for(const auto& packet:fixture.snapshot.packets) REQUIRE(mux.write(packet).is_success());
  mux.abort();
  const auto partial=std::filesystem::path(destination.wstring()+L".partial");
  REQUIRE(mux.recovery_path()==partial);
  REQUIRE(std::filesystem::exists(partial));
  const auto evidence=probe(partial,fixture.artifacts);
  REQUIRE(evidence["streams"].size()==4);
  for(std::size_t i=0;i<4;++i) {
   const auto& tags=evidence["streams"][i]["tags"];
   const auto title=tags.contains("title")?tags["title"]:tags["handler_name"];
   CHECK(title==(i==0?fixture.descriptors[0].title:stream_title(fixture.descriptors[i].kind)));
  }
  std::array<std::size_t,4> expected{},actual{};
  std::array<std::vector<double>,4> expected_pts,expected_dts;
  const auto origin=fixture.snapshot.packets.front().dts;
  const double frequency=static_cast<double>(QpcClock{}.frequency());
  std::size_t decoded=0,bframes=0;
  for(const auto& packet:fixture.snapshot.packets) {
   const auto stream=static_cast<std::size_t>(packet.stream);
   ++expected[stream];
   expected_pts[stream].push_back(static_cast<double>(packet.pts-origin)/frequency);
   expected_dts[stream].push_back(static_cast<double>(packet.dts-origin)/frequency);
  }
  for(const auto& item:evidence["packets_and_frames"]) {
   if(item["type"]=="packet") {
    const auto stream=item["stream_index"].get<std::size_t>();
    const auto index=actual[stream]++;
    REQUIRE(index<expected_pts[stream].size());
    const auto tolerance=container==Container::Mp4?.000022:.0011;
    CHECK(std::abs(std::stod(item["pts_time"].get<std::string>())-expected_pts[stream][index])<=tolerance);
    if(item.contains("dts_time") && item["dts_time"].is_string())
     CHECK(std::abs(std::stod(item["dts_time"].get<std::string>())-expected_dts[stream][index])<=tolerance);
   }
   if(item["type"]=="frame" && item["media_type"]=="video") {
    ++decoded; if(item["pict_type"]=="B") ++bframes;
   }
  }
  REQUIRE(actual==expected);
  REQUIRE(decoded==expected[0]);
  REQUIRE(bframes>0);
 }
}

TEST_CASE("video-only replay muxes without disabled audio streams") {
 auto fixture=generate_fixture(false); FfmpegClipMuxer mux;
 const auto path=fixture.artifacts/"video-only.mp4";
 const auto result=mux.write(fixture.snapshot,fixture.descriptors,path,Container::Mp4);
 if(!result.is_success()) INFO(result.error().message);
 REQUIRE(result.is_success());
 const auto actual=probe(path,fixture.artifacts);
 REQUIRE(actual["streams"].size()==1);
 REQUIRE(actual["streams"][0]["codec_name"]=="h264");
}

// Hidden helper runs only in the subprocess below. Its muxers stay alive until
// TerminateProcess: no destructor, abort or trailer can repair the output.
TEST_CASE("continuous crash writer child", "[.continuous-crash-child]") {
 const auto directory=std::filesystem::current_path();
 REQUIRE(directory.filename().wstring().starts_with(L"continuous-crash-{"));
 auto fixture=generate_fixture();
 FfmpegContinuousMuxer mp4,mkv;
 REQUIRE(mp4.open(fixture.descriptors,directory/L"crashed.mp4",Container::Mp4).is_success());
 REQUIRE(mkv.open(fixture.descriptors,directory/L"crashed.mkv",Container::Mkv).is_success());
 const auto origin=fixture.snapshot.packets.front().dts;
 QpcTicks span=0;
 // Join GOPs on the decode timeline; presentation tails already include
 // reordered frames and would introduce artificial gaps between repetitions.
 for(const auto& packet:fixture.snapshot.packets) if(packet.stream==StreamKind::Video)
  span=(std::max)(span,packet.dts+packet.duration-origin);
 json expected={{"frequency",QpcClock{}.frequency()},
  {"pts",json::array({json::array(),json::array(),json::array(),json::array()})},
  {"dts",json::array({json::array(),json::array(),json::array(),json::array()})}};
 for(int cycle=0;cycle<12;++cycle) for(auto packet:fixture.snapshot.packets) {
  packet.pts+=span*cycle; packet.dts+=span*cycle;
  REQUIRE(mp4.write(packet).is_success());
  REQUIRE(mkv.write(packet).is_success());
  expected["pts"][static_cast<std::size_t>(packet.stream)].push_back(packet.pts-origin);
  expected["dts"][static_cast<std::size_t>(packet.stream)].push_back(packet.dts-origin);
 }
 {std::ofstream manifest(directory/L"ready.tmp"); manifest<<expected.dump(); manifest.close(); REQUIRE(manifest.good());}
 std::filesystem::rename(directory/L"ready.tmp",directory/L"ready.json");
 Sleep(INFINITE);
 FAIL("The crash writer must be terminated by its parent");
}

TEST_CASE("continuous fragments survive process termination without trailer") {
 GUID guid{}; REQUIRE(SUCCEEDED(CoCreateGuid(&guid))); wchar_t name[40]{}; StringFromGUID2(guid,name,40);
 const auto directory=std::filesystem::path(REBELLIOCAP_SOURCE_DIR)/"artifacts"/(std::wstring(L"continuous-crash-")+name);
 REQUIRE(std::filesystem::create_directories(directory));
 wchar_t executable[MAX_PATH]{}; const auto path_size=GetModuleFileNameW(nullptr,executable,MAX_PATH);
 REQUIRE(path_size>0); REQUIRE(path_size<MAX_PATH);
 std::wstring command=L"\""+std::wstring(executable)+L"\" \"continuous crash writer child\"";
 STARTUPINFOW startup{}; startup.cb=sizeof(startup);
 struct Child {
  PROCESS_INFORMATION process{};
  ~Child() {if(process.hProcess){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);CloseHandle(process.hThread);}}
 } child;
 REQUIRE(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&child.process));
 const auto deadline=GetTickCount64()+30000;
 while(!std::filesystem::exists(directory/L"ready.json") && GetTickCount64()<deadline) {
  REQUIRE(WaitForSingleObject(child.process.hProcess,20)==WAIT_TIMEOUT);
 }
 REQUIRE(std::filesystem::exists(directory/L"ready.json"));
 REQUIRE(TerminateProcess(child.process.hProcess,99));
 REQUIRE(WaitForSingleObject(child.process.hProcess,5000)==WAIT_OBJECT_0);
 DWORD exit_code=0; REQUIRE(GetExitCodeProcess(child.process.hProcess,&exit_code)); REQUIRE(exit_code==99);
 std::ifstream manifest(directory/L"ready.json"); json expected; manifest>>expected;
 const auto frequency=expected["frequency"].get<double>();
 for(const auto extension:{L"mp4",L"mkv"}) {
  const auto file=directory/(std::wstring(L"crashed.")+extension+L".partial");
  REQUIRE(std::filesystem::file_size(file)>0);
  const auto evidence=probe(file,directory);
  REQUIRE(evidence["streams"].size()==4);
  const auto tolerance=std::wstring_view(extension)==L"mp4"?.000022:.0011;
  std::array<std::size_t,4> packets{},frames{}; std::size_t bframes=0;
  std::array<double,4> decoded_end{};
  for(const auto& item:evidence["packets_and_frames"]) {
   const auto stream=item["stream_index"].get<std::size_t>(); REQUIRE(stream<4);
   if(item["type"]=="packet") {
    const auto index=packets[stream]++; REQUIRE(index<expected["pts"][stream].size());
    const auto pts=expected["pts"][stream][index].get<double>()/frequency;
    CHECK(std::abs(std::stod(item["pts_time"].get<std::string>())-pts)<=tolerance);
    if(item.contains("dts_time") && item["dts_time"].is_string()) {
     const auto dts=expected["dts"][stream][index].get<double>()/frequency;
     CHECK(std::abs(std::stod(item["dts_time"].get<std::string>())-dts)<=tolerance);
    }
   } else if(item["type"]=="frame") {
    ++frames[stream]; if(stream==0 && item["pict_type"]=="B") ++bframes;
    decoded_end[stream]=(std::max)(decoded_end[stream],std::stod(item["best_effort_timestamp_time"].get<std::string>()));
   }
  }
  for(std::size_t stream=0;stream<4;++stream) {CHECK(packets[stream]>0);CHECK(frames[stream]>0);CHECK(decoded_end[stream]>=5.0);}
  CHECK(frames[0]==packets[0]); CHECK(bframes>0);
  CHECK(packets[0]<=expected["pts"][0].size());
 }
 std::cout<<"CRASH_ARTIFACTS="<<directory.string()<<'\n';
}

TEST_CASE("owned sibling lifecycle preserves foreign files and rejects publication race") {
 auto fixture=generate_fixture(); FfmpegClipMuxer mux;
 const auto final=fixture.artifacts/L"гонка.mp4";
 const auto foreign=fixture.artifacts/L"гонка.mp4.rebelliocap-foreign.tmp";
 {std::ofstream sentinel(foreign);sentinel<<"foreign";}
 std::filesystem::path owned; bool created=false,validated=false;
 FfmpegMuxTestAccess::observe(mux,[&](const auto& temp,bool publishing) {
  owned=temp;CHECK(temp.parent_path()==final.parent_path());CHECK(std::filesystem::exists(temp));
  if(!publishing) {created=true;CHECK_FALSE(std::filesystem::exists(final));}
  else {validated=true;CHECK(std::filesystem::file_size(temp)>0);std::ofstream winner(final);winner<<"race winner";}
 });
 auto raced=mux.write(fixture.snapshot,fixture.descriptors,final,Container::Mp4);
 CHECK(created);CHECK(validated);REQUIRE_FALSE(raced.is_success());CHECK(raced.error().code=="mux.destination_exists");CHECK_FALSE(std::filesystem::exists(owned));
 {std::ifstream file(final);std::string line;std::getline(file,line);CHECK(line=="race winner");}
 {std::ifstream file(foreign);std::string line;std::getline(file,line);CHECK(line=="foreign");}
 const auto failure_final=fixture.artifacts/L"failure.mkv";
 FfmpegMuxTestAccess::observe(mux,[&](const auto& temp,bool publishing) {owned=temp;if(publishing)throw std::runtime_error("injected validation-to-publication failure");});
 auto failed=mux.write(fixture.snapshot,fixture.descriptors,failure_final,Container::Mkv);
 REQUIRE_FALSE(failed.is_success());CHECK_FALSE(std::filesystem::exists(failure_final));CHECK_FALSE(std::filesystem::exists(owned));CHECK(std::filesystem::exists(foreign));
 auto invalid=fixture.snapshot;
 for(auto& p:invalid.packets)if(p.stream==StreamKind::Video && p.pts>invalid.end){p.pts=invalid.end+3*p.duration;break;}
 auto rejected=mux.write(invalid,fixture.descriptors,failure_final,Container::Mkv);REQUIRE_FALSE(rejected.is_success());CHECK(rejected.error().code=="mux.invalid_snapshot");
}
