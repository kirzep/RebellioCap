// Developer-only offline AVIO screening. Uses a pre-encoded owned synthetic
//240-frame H.264 fixture; never captures the desktop or changes runtime defaults.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/pipeline_telemetry.h"
#include "mux/ffmpeg_continuous_muxer.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
using namespace rebelliocap;
using json=nlohmann::json;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T> void require_result(const Result<T>& result){if(!result.is_success())throw std::runtime_error(result.error().code+": "+result.error().message);}
std::uint64_t ticks(FILETIME t){return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
std::uint64_t cpu_time(){FILETIME c{},e{},k{},u{};require(GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)!=0,"Process CPU counters");return ticks(k)+ticks(u);}
IO_COUNTERS io_counters(){IO_COUNTERS result{};require(GetProcessIoCounters(GetCurrentProcess(),&result)!=0,"Process IO counters");return result;}
std::string utf8(const std::filesystem::path& path){const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
}
int wmain(int argc,wchar_t** argv){
 try {
  require(argc==3,"Usage: rebelliocap_avio_probe synthetic240frames.mp4 fresh-output-directory");
  const std::filesystem::path input(argv[1]),root(argv[2]);
  require(input.is_absolute()&&root.is_absolute(),"Use absolute evidence paths");
  require(!std::filesystem::exists(root),"Refusing to overwrite evidence directory");
  AVFormatContext* raw=nullptr;const auto input_name=utf8(input);
  require(avformat_open_input(&raw,input_name.c_str(),nullptr,nullptr)>=0,"Open synthetic fixture");
  const auto close=[](AVFormatContext* p){avformat_close_input(&p);};
  std::unique_ptr<AVFormatContext,decltype(close)> format(raw,close);
  require(avformat_find_stream_info(raw,nullptr)>=0,"Read fixture stream info");
  require(raw->nb_streams==1,"Expected one synthetic video stream");
  const auto* source=raw->streams[0];const auto* codec=source->codecpar;
  require(codec->codec_id==AV_CODEC_ID_H264&&codec->codec_type==AVMEDIA_TYPE_VIDEO&&codec->extradata_size>0&&codec->width>0&&codec->height>0&&codec->width<=8192&&codec->height<=8192,"Expected bounded H264 configuration");
  QpcClock clock;require(clock.frequency()>0&&clock.frequency()<=INT32_MAX,"QPC time base");
  StreamDescriptor descriptor{StreamKind::Video,"h264","Synthetic AVIO workload",{}, {1,static_cast<std::int32_t>(clock.frequency())},static_cast<std::uint32_t>(codec->width),static_cast<std::uint32_t>(codec->height)};
  for(int i=0;i<codec->extradata_size;++i)descriptor.codec_extradata.push_back(static_cast<std::byte>(codec->extradata[i]));
  const auto free_packet=[](AVPacket* p){av_packet_free(&p);};
  std::unique_ptr<AVPacket,decltype(free_packet)> packet(av_packet_alloc(),free_packet);
  require(packet!=nullptr,"Allocate demux packet");
  std::vector<EncodedPacket> packets;std::uint64_t payload_bytes=0;QpcTicks period=0,previous_dts=INT64_MIN;
  int read=0;
  while((read=av_read_frame(raw,packet.get()))>=0){
    require(packet->stream_index==0&&packet->pts!=AV_NOPTS_VALUE&&packet->dts!=AV_NOPTS_VALUE&&packet->duration>0,"Fixture packet timing");
    require(packet->size>0&&packet->size<64*1024*1024&&packets.size()<240,"Bounded fixture payload");
    auto data=std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(packet->size));
    std::memcpy(data->data(),packet->data,data->size());payload_bytes+=data->size();
    const AVRational target{1,static_cast<int>(clock.frequency())};
    EncodedPacket p{StreamKind::Video,1,av_rescale_q(packet->pts,source->time_base,target),av_rescale_q(packet->dts,source->time_base,target),av_rescale_q(packet->duration,source->time_base,target),(packet->flags&AV_PKT_FLAG_KEY)!=0,std::move(data)};
    require(p.dts>previous_dts,"Strict input DTS");previous_dts=p.dts;period=(std::max)(period,p.pts+p.duration);
    packets.push_back(std::move(p));av_packet_unref(packet.get());
  }
  require(read==AVERROR_EOF&&packets.size()==240&&packets.front().keyframe,"Complete240-frame IDR fixture");
  const auto origin=packets.front().pts;
  for(auto& p:packets){p.pts-=origin;p.dts-=origin;}
  period-=origin;
  require(std::abs(period-clock.frequency()*4)<clock.frequency()/1000,"Expected four-second fixture");
  require(std::filesystem::create_directories(root),"Create fresh evidence directory");
  json report={{"input",input_name},{"input_packets",packets.size()},{"payload_bytes_per_repeat",payload_bytes},{"repeats",15},{"output_packets_per_file",packets.size()*15},{"media_seconds",static_cast<double>(period)*15/static_cast<double>(clock.frequency())},
   {"scope","offline synchronous mux only; concurrent functional soak; process IO counts/mean transfer, not per-write size distribution, ETW or game FPS"},{"runs",json::array()}};
  const std::array<std::size_t,5> sizes{32*1024,128*1024,256*1024,512*1024,1024*1024};
  for(int round=0;round<3;++round)for(std::size_t index=0;index<sizes.size();++index){
    const auto size=sizes[round==1?sizes.size()-1-index:index];
    const auto id="round"+std::to_string(round+1)+"-"+std::to_string(size/1024)+"k";
    auto telemetry=std::make_shared<PipelineTelemetry>();telemetry->configure(true);
    FfmpegContinuousMuxer mux(size,telemetry);
    const auto io_before=io_counters();const auto cpu_before=cpu_time();const auto started=std::chrono::steady_clock::now();
    require_result(mux.open({descriptor},root/(id+".mp4"),Container::Mp4));
    for(int repeat=0;repeat<15;++repeat)for(const auto& original:packets){
      auto p=original;p.pts+=period*repeat;p.dts+=period*repeat;require_result(mux.write(p));
    }
    const auto final_started=std::chrono::steady_clock::now();require_result(mux.finalize());
    const auto ended=std::chrono::steady_clock::now();const auto cpu_after=cpu_time();const auto io_after=io_counters();
    const auto disk=telemetry->snapshot().stages[static_cast<std::size_t>(PipelineStage::DiskWriteCpu)];
    const auto writes=io_after.WriteOperationCount-io_before.WriteOperationCount;
    const auto bytes=io_after.WriteTransferCount-io_before.WriteTransferCount;
    require(disk.count>0&&writes>0,"Observed disk callbacks and process writes");
    report["runs"].push_back({{"id",id},{"avio_bytes",size},{"file_bytes",std::filesystem::file_size(root/(id+".mp4"))},{"process_write_operations",writes},{"process_write_bytes",bytes},{"mean_process_transfer_bytes",static_cast<double>(bytes)/static_cast<double>(writes)},{"wall_ms",std::chrono::duration<double,std::milli>(ended-started).count()},{"finalize_ms",std::chrono::duration<double,std::milli>(ended-final_started).count()},{"cpu_ms",static_cast<double>(cpu_after-cpu_before)/10000},
      {"write_callback",{{"count",disk.count},{"p50_us",disk.p50_us},{"p95_us",disk.p95_us},{"p99_us",disk.p99_us},{"p999_us",disk.p999_us},{"max_us",disk.max_us}}}});
  }
  std::ofstream result(root/"results.json");result<<report.dump(2);require(result.good(),"Write report");
  std::cout<<report.dump(2)<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
