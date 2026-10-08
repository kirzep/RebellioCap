#include "mux/ffmpeg_clip_muxer.h"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <map>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
namespace rebelliocap {
namespace {
struct Failure { std::string code, message; };
void require(bool ok, const char* message) { if (!ok) throw Failure{"mux.invalid_snapshot",message}; }
void avcheck(int rc, const char* message) { if(rc < 0) { char b[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(rc,b,sizeof(b)); throw Failure{"mux.ffmpeg",std::string(message)+": "+b}; } }
struct File {
 HANDLE handle=INVALID_HANDLE_VALUE;
 std::filesystem::path path;
 bool owned=false;
 ~File() { close(); if(owned) DeleteFileW(path.c_str()); }
 void close() { if(handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle); handle=INVALID_HANDLE_VALUE; } }
};
int write_bytes(void* opaque,const std::uint8_t* bytes,int count) {
 DWORD written=0;
 if(!WriteFile(static_cast<File*>(opaque)->handle,bytes,static_cast<DWORD>(count),&written,nullptr) || written!=static_cast<DWORD>(count)) return AVERROR(EIO);
 return count;
}
int read_bytes(void* opaque,std::uint8_t* bytes,int count) {
 DWORD read=0;
 if(!ReadFile(static_cast<File*>(opaque)->handle,bytes,static_cast<DWORD>(count),&read,nullptr)) return AVERROR(EIO);
 return read ? static_cast<int>(read) : AVERROR_EOF;
}
std::int64_t seek_bytes(void* opaque,std::int64_t offset,int whence) {
 auto handle=static_cast<File*>(opaque)->handle; LARGE_INTEGER value{},result{}; value.QuadPart=offset;
 if(whence==AVSEEK_SIZE) return GetFileSizeEx(handle,&result) ? result.QuadPart : AVERROR(EIO);
 whence &= ~AVSEEK_FORCE;
 DWORD method=whence==SEEK_SET?FILE_BEGIN:whence==SEEK_CUR?FILE_CURRENT:FILE_END;
 if(whence!=SEEK_SET && whence!=SEEK_CUR && whence!=SEEK_END) return AVERROR(EINVAL);
 return SetFilePointerEx(handle,value,&result,method) ? result.QuadPart : AVERROR(EIO);
}
struct Io {
 AVIOContext* value=nullptr;
 Io(File& file,bool writing) {
  auto* buffer=static_cast<unsigned char*>(av_malloc(32768));
  if(!buffer) throw std::bad_alloc();
  value=avio_alloc_context(buffer,32768,writing?1:0,&file,writing?nullptr:read_bytes,writing?write_bytes:nullptr,seek_bytes);
  if(!value) { av_free(buffer); throw std::bad_alloc(); }
 }
 ~Io() { if(value) { av_freep(&value->buffer); avio_context_free(&value); } }
};
struct Output { AVFormatContext* value=nullptr; ~Output() { avformat_free_context(value); } };
struct Input { AVFormatContext* value=nullptr; ~Input() { if(value) avformat_close_input(&value); } };
struct Packet { AVPacket* value=av_packet_alloc(); ~Packet() { av_packet_free(&value); } };
constexpr std::array kinds{StreamKind::Video,StreamKind::MixedAudio,StreamKind::SystemAudio,StreamKind::MicrophoneAudio};
int index_of(StreamKind kind) { for(int i=0;i<4;++i) if(kinds[static_cast<std::size_t>(i)]==kind) return i; return -1; }
void metadata(AVDictionary** dict,const char* key,std::int64_t value) { avcheck(av_dict_set(dict,key,std::to_string(value).c_str(),0),"metadata"); }
}
Result<std::filesystem::path> FfmpegClipMuxer::write(const ReplaySnapshot& snapshot,
 const std::vector<StreamDescriptor>& descriptions,const std::filesystem::path& destination,Container container) {
 try {
  const auto final=std::filesystem::absolute(destination);
  if(GetFileAttributesW(final.c_str())!=INVALID_FILE_ATTRIBUTES)
   return Result<std::filesystem::path>::failure({"mux.destination_exists","Destination already exists",{}});
  require(container==Container::Mp4 || container==Container::Mkv,"Unknown container");
  require(snapshot.actual_start>=0 && snapshot.requested_start>=snapshot.actual_start && snapshot.end>=snapshot.requested_start,"Invalid presentation bounds");
  require(!descriptions.empty() && descriptions.size()<=kinds.size() && !snapshot.packets.empty(),"Descriptors and packets are required");
  const auto frequency=QpcClock{}.frequency();
  require(frequency>0 && frequency<=INT_MAX,"Unsupported QPC frequency");
  const AVRational qpc{1,static_cast<int>(frequency)};
  std::array<const StreamDescriptor*,4> desc{};
  for(const auto& d:descriptions) {
   const auto i=index_of(d.kind); require(i>=0 && !desc[static_cast<std::size_t>(i)],"Duplicate or unknown stream");
   require(d.time_base.numerator==1 && d.time_base.denominator==frequency,"Descriptor must use current raw QPC time base");
   require(!d.codec_extradata.empty() && d.codec_extradata.size()<=INT_MAX-AV_INPUT_BUFFER_PADDING_SIZE,"Missing or excessive extradata");
   require(d.kind==StreamKind::Video ? d.codec=="h264" && d.width>0 && d.height>0 && d.width<=INT_MAX && d.height<=INT_MAX : d.codec=="aac" && d.sample_rate>0 && d.sample_rate<=INT_MAX && d.channels==2,"Invalid codec parameters");
   desc[static_cast<std::size_t>(i)]=&d;
  }
  require(desc[0]!=nullptr,"Video descriptor is required");
  std::array<std::size_t,4> counts{};
  std::array<QpcTicks,4> last_dts{}, first_duration{};
  QpcTicks actual_end=snapshot.actual_start,video_end=snapshot.actual_start,tail=0;
  QpcTicks frame_duration=0;
  for(const auto& p:snapshot.packets) {
   const auto i=index_of(p.stream); require(i>=0,"Unknown packet stream"); const auto n=static_cast<std::size_t>(i);
   require(desc[n]!=nullptr,"Packet stream has no descriptor");
   require(p.epoch==snapshot.epoch && p.payload && !p.payload->empty() && p.payload->size()<=INT_MAX,"Invalid packet data or epoch");
   require(p.duration>0 && p.pts>=snapshot.actual_start && p.pts<=INT64_MAX-p.duration && p.dts<=p.pts && p.dts>=-INT64_MAX+snapshot.actual_start,"Invalid packet timestamps");
   require(counts[n]==0 || p.dts>last_dts[n],"Nonmonotonic stream DTS");
   if(counts[n]==0) first_duration[n]=p.duration;
   last_dts[n]=p.dts; ++counts[n];
   actual_end=(std::max)(actual_end,p.pts+p.duration);
   if(p.stream==StreamKind::Video) {
    if(!frame_duration) { require(p.keyframe && p.pts==snapshot.actual_start,"Video must start at an IDR"); frame_duration=p.duration; }
    require(frame_duration==p.duration && frame_duration<=INT64_MAX/2,"Variable video duration not supported");
    require(p.pts<=snapshot.end || p.pts-snapshot.end<=2*frame_duration,"Video dependency tail exceeds two B-frame reorder bound");
    tail=(std::max)(tail,p.pts-snapshot.end); video_end=(std::max)(video_end,p.pts+p.duration);
   } else require(p.pts<=snapshot.end,"Audio begins beyond requested cutoff");
  }
  for(std::size_t i=0;i<desc.size();++i) if(desc[i]) require(counts[i]>0,"Every described stream must have packets");
  File file;
  for(int attempt=0;attempt<8;++attempt) {
   GUID id{}; if(FAILED(CoCreateGuid(&id))) throw Failure{"mux.temp_failed","Could not generate temp identity"};
   wchar_t name[40]{}; StringFromGUID2(id,name,40);
   file.path=final.parent_path()/(final.filename().wstring()+L".rebelliocap-"+name+L".tmp");
   file.handle=CreateFileW(file.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
   if(file.handle!=INVALID_HANDLE_VALUE) {file.owned=true;break;}
   if(GetLastError()!=ERROR_FILE_EXISTS) break;
  }
  if(!file.owned) throw Failure{"mux.temp_failed","Could not exclusively create owned sibling temp"};
  if(lifecycle_observer_) lifecycle_observer_(file.path,false);
  std::array<int,4> output_indices{}; output_indices.fill(-1);
  std::vector<std::size_t> output_kinds;
  {
   Io io(file,true); Output output;
   avcheck(avformat_alloc_output_context2(&output.value,nullptr,container==Container::Mp4?"mp4":"matroska",nullptr),"allocate container");
   require(output.value!=nullptr,"Missing muxer"); auto* ctx=output.value; ctx->pb=io.value; ctx->flags|=AVFMT_FLAG_CUSTOM_IO;
   ctx->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
   metadata(&ctx->metadata,"rebelliocap_requested_start_qpc",snapshot.requested_start-snapshot.actual_start);
   metadata(&ctx->metadata,"rebelliocap_requested_end_qpc",snapshot.end-snapshot.actual_start);
   metadata(&ctx->metadata,"rebelliocap_actual_start_qpc",0);
   metadata(&ctx->metadata,"rebelliocap_actual_end_qpc",actual_end-snapshot.actual_start);
   metadata(&ctx->metadata,"rebelliocap_video_end_qpc",video_end-snapshot.actual_start);
   metadata(&ctx->metadata,"rebelliocap_dependency_tail_qpc",tail);
   metadata(&ctx->metadata,"rebelliocap_qpc_frequency",frequency);
   for(std::size_t i=0;i<desc.size();++i) {
    if(!desc[i]) continue;
    auto* stream=avformat_new_stream(ctx,nullptr); if(!stream) throw std::bad_alloc();
    output_indices[i]=stream->index; output_kinds.push_back(i);
    stream->id=static_cast<int>(i); const auto& d=*desc[i]; auto* c=stream->codecpar;
    c->codec_type=i==0?AVMEDIA_TYPE_VIDEO:AVMEDIA_TYPE_AUDIO; c->codec_id=i==0?AV_CODEC_ID_H264:AV_CODEC_ID_AAC;
    c->extradata=static_cast<std::uint8_t*>(av_mallocz(d.codec_extradata.size()+AV_INPUT_BUFFER_PADDING_SIZE)); if(!c->extradata) throw std::bad_alloc();
    std::memcpy(c->extradata,d.codec_extradata.data(),d.codec_extradata.size()); c->extradata_size=static_cast<int>(d.codec_extradata.size());
    if(i==0) {c->width=static_cast<int>(d.width);c->height=static_cast<int>(d.height);stream->time_base={1,90000};}
    else {c->sample_rate=static_cast<int>(d.sample_rate);c->frame_size=static_cast<int>(av_rescale_q(first_duration[i],qpc,{1,c->sample_rate}));av_channel_layout_default(&c->ch_layout,static_cast<int>(d.channels));stream->time_base={1,c->sample_rate};}
    // MP4 track title emits a raw udta/name string that FFmpeg retries as
    // malformed length-prefixed metadata. hdlr retains the same track name.
    if(container==Container::Mkv)
     avcheck(av_dict_set(&stream->metadata,"title",stream_title(d.kind),0),"title");
    avcheck(av_dict_set(&stream->metadata,"handler_name",stream_title(d.kind),0),"handler title");
   }
   AVDictionary* options=nullptr;
   if(container==Container::Mp4) av_dict_set(&options,"movflags","use_metadata_tags",0);
   const int header=avformat_write_header(ctx,&options); av_dict_free(&options); avcheck(header,"write header");
   Packet packet; if(!packet.value) throw std::bad_alloc();
   for(const auto& p:snapshot.packets) {
    auto* out=packet.value; avcheck(av_new_packet(out,static_cast<int>(p.payload->size())),"packet allocation");
    std::memcpy(out->data,p.payload->data(),p.payload->size()); out->stream_index=output_indices[static_cast<std::size_t>(index_of(p.stream))];
    out->pts=p.pts-snapshot.actual_start;out->dts=p.dts-snapshot.actual_start;out->duration=p.duration;out->pos=-1;
    if(p.keyframe) out->flags|=AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(out,qpc,ctx->streams[out->stream_index]->time_base);
    avcheck(av_interleaved_write_frame(ctx,out),"interleave packet"); av_packet_unref(out);
   }
   avcheck(av_write_trailer(ctx),"write trailer"); avio_flush(io.value); avcheck(io.value->error,"flush AVIO");
   if(!FlushFileBuffers(file.handle)) throw Failure{"mux.flush_failed","Durable file flush failed"};
  }
  // Reopen through the same owned Windows handle: UTF-16 paths never pass
  // through FFmpeg's narrow path API. No codec is opened by this validation.
  if(seek_bytes(&file,0,SEEK_SET)<0) throw Failure{"mux.validation_failed","Rewind failed"};
  {
   Io io(file,false); Input input; input.value=avformat_alloc_context();if(!input.value) throw std::bad_alloc();
   input.value->pb=io.value;input.value->flags|=AVFMT_FLAG_CUSTOM_IO;
   avcheck(avformat_open_input(&input.value,nullptr,nullptr,nullptr),"validate container header");
   require(input.value->nb_streams==output_kinds.size(),"Validation stream count mismatch");
   for(std::size_t i=0;i<output_kinds.size();++i) require(input.value->streams[i]->codecpar->codec_id==(output_kinds[i]==0?AV_CODEC_ID_H264:AV_CODEC_ID_AAC),"Validation codec mismatch");
   auto* cutoff=av_dict_get(input.value->metadata,"rebelliocap_requested_end_qpc",nullptr,0);
   require(cutoff && cutoff->value==std::to_string(snapshot.end-snapshot.actual_start),"Presentation metadata did not survive mux");
   std::array<std::size_t,4> observed{}; Packet packet;if(!packet.value) throw std::bad_alloc(); int rc=0;
   while((rc=av_read_frame(input.value,packet.value))>=0) {
    require(packet.value->stream_index>=0 && static_cast<std::size_t>(packet.value->stream_index)<output_kinds.size() && packet.value->size>0,"Invalid output packet");
    ++observed[output_kinds[static_cast<std::size_t>(packet.value->stream_index)]];av_packet_unref(packet.value);
   }
   require(rc==AVERROR_EOF && observed==counts,"Output packet count or read integrity mismatch");
  }
  if(lifecycle_observer_) lifecycle_observer_(file.path,true);
  file.close();
  if(!MoveFileExW(file.path.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH)) {
   const auto code=GetLastError();
   throw Failure{code==ERROR_ALREADY_EXISTS || code==ERROR_FILE_EXISTS?"mux.destination_exists":"mux.publish_failed","Atomic no-replace publication failed"};
  }
  file.owned=false;
  return Result<std::filesystem::path>::success(final);
 } catch(const Failure& f) { return Result<std::filesystem::path>::failure({f.code,f.message,{}}); }
 catch(const std::exception& e) {return Result<std::filesystem::path>::failure({"mux.exception",e.what(),{}});}
}
}
