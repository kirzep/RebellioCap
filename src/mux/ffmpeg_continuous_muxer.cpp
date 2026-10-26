#include "mux/ffmpeg_continuous_muxer.h"
#include "core/pipeline_telemetry.h"

#include <Windows.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/buffer.h>
#include <libavutil/mem.h>
}

namespace rebelliocap {
namespace {

// The copied FFmpeg buffer keeps the original budget owner through interleaving.
void release_packet_copy(void* opaque, std::uint8_t* data) {
  av_free(data);
  delete static_cast<std::shared_ptr<const std::vector<std::byte>>*>(opaque);
}

struct Failure {
  std::string code;
  std::string message;
};

void require(bool condition, const char* message) {
  if (!condition) throw Failure{"mux.invalid_stream", message};
}

void avcheck(int result, const char* action) {
  if (result >= 0) return;
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, text, sizeof(text));
  throw Failure{"mux.ffmpeg", std::string(action) + ": " + text};
}

struct DiskIoState { HANDLE file{INVALID_HANDLE_VALUE}; std::shared_ptr<PipelineTelemetry> telemetry; };

int write_bytes(void* opaque, const std::uint8_t* bytes, int count) {
  const auto& io = *static_cast<DiskIoState*>(opaque);
  const auto handle = io.file;
  PipelineTimingScope timing(io.telemetry.get(), PipelineStage::DiskWriteCpu);
  DWORD written = 0;
  if (!WriteFile(handle, bytes, static_cast<DWORD>(count), &written, nullptr) ||
      written != static_cast<DWORD>(count)) {
    return AVERROR(EIO);
  }
  return count;
}

std::int64_t seek_bytes(void* opaque, std::int64_t offset, int whence) {
  const auto handle = static_cast<DiskIoState*>(opaque)->file;
  LARGE_INTEGER distance{};
  LARGE_INTEGER result{};
  distance.QuadPart = offset;
  if (whence == AVSEEK_SIZE) {
    return GetFileSizeEx(handle, &result) ? result.QuadPart : AVERROR(EIO);
  }
  whence &= ~AVSEEK_FORCE;
  const DWORD method = whence == SEEK_SET ? FILE_BEGIN
      : whence == SEEK_CUR ? FILE_CURRENT
                           : FILE_END;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    return AVERROR(EINVAL);
  }
  return SetFilePointerEx(handle, distance, &result, method)
      ? result.QuadPart
      : AVERROR(EIO);
}

int kind_index(StreamKind kind) {
  switch (kind) {
    case StreamKind::Video: return 0;
    case StreamKind::MixedAudio: return 1;
    case StreamKind::SystemAudio: return 2;
    case StreamKind::MicrophoneAudio: return 3;
  }
  return -1;
}

}  // namespace

struct FfmpegContinuousMuxer::Impl {
  explicit Impl(std::size_t bytes, std::shared_ptr<PipelineTelemetry> telemetry)
      : avio_buffer_bytes(bytes) { disk_io.telemetry=std::move(telemetry); }
  const std::size_t avio_buffer_bytes;
  DiskIoState disk_io;
  AVPacket* output{nullptr};
  mutable std::mutex mutex;
  HANDLE file{INVALID_HANDLE_VALUE};
  std::filesystem::path final_path;
  std::filesystem::path partial_path;
  bool owns_partial{false};
  AVFormatContext* context{nullptr};
  AVIOContext* io{nullptr};
  std::array<int, 4> stream_indices{-1, -1, -1, -1};
  std::array<AVRational, 4> source_time_bases{};
  std::optional<QpcTicks> origin;
  bool opened{false};
  std::uint64_t media_packets{0};
  bool trailer_attempted{false};
  std::filesystem::path retained_path;

  void close_context() noexcept {
    av_packet_free(&output);
    if (context != nullptr) {
      context->pb = nullptr;
      avformat_free_context(context);
      context = nullptr;
    }
    if (io != nullptr) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      file = INVALID_HANDLE_VALUE;
    }
    opened = false;
    origin.reset();
    stream_indices.fill(-1);
  }

  void abort_locked() noexcept {
    const bool retain = owns_partial && media_packets > 0;
    // Queue overflow/validation failure may leave healthy IO. Drain muxer
    // buffers once; on disk failure keep already completed fragments instead.
    if (retain && opened && context && io && io->error >= 0 && !trailer_attempted) {
      trailer_attempted = true;
      static_cast<void>(av_write_trailer(context));
      avio_flush(io);
      static_cast<void>(FlushFileBuffers(file));
    }
    close_context();
    if (owns_partial) {
      if (retain) retained_path.swap(partial_path);
      else DeleteFileW(partial_path.c_str());
      owns_partial = false;
    }
    final_path.clear();
    partial_path.clear();
    media_packets = 0;
    trailer_attempted = false;
  }

  Result<void> failure(Failure error) {
    return Result<void>::failure(
        {.code = std::move(error.code), .message = std::move(error.message), .hresult = std::nullopt});
  }
};

FfmpegContinuousMuxer::FfmpegContinuousMuxer(std::size_t avio_buffer_bytes, std::shared_ptr<PipelineTelemetry> telemetry)
    : implementation_(std::make_unique<Impl>(avio_buffer_bytes, std::move(telemetry))) {}

FfmpegContinuousMuxer::~FfmpegContinuousMuxer() { abort(); }

Result<void> FfmpegContinuousMuxer::open(
    const std::vector<StreamDescriptor>& descriptors,
    const std::filesystem::path& destination, Container container) {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  if (state.opened || state.owns_partial) {
    return Result<void>::failure(
        {"mux.already_open", "A continuous destination is already open.", {}});
  }
  try {
    require(state.avio_buffer_bytes > 0 && state.avio_buffer_bytes <= static_cast<std::size_t>(INT_MAX),
            "AVIO buffer size must be positive and fit FFmpeg's integer size");
    require(container == Container::Mp4 || container == Container::Mkv,
            "Unknown output container");
    state.final_path = std::filesystem::absolute(destination);
    state.partial_path = state.final_path.parent_path() /
        (state.final_path.filename().wstring() + L".partial");
    if (GetFileAttributesW(state.final_path.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(state.partial_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      throw Failure{"mux.destination_exists",
                    "The destination or its sibling partial file already exists."};
    }
    require(!descriptors.empty() && descriptors.size() <= 4,
            "One to four stream descriptors are required");
    state.file = CreateFileW(state.partial_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state.file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        throw Failure{"mux.destination_exists", "The sibling partial file already exists."};
      }
      throw Failure{"mux.temp_failed", "Could not create the sibling partial file."};
    }
    state.owns_partial = true;
    state.retained_path.clear();
    state.media_packets = 0;
    state.trailer_attempted = false;

    auto* buffer = static_cast<unsigned char*>(av_malloc(state.avio_buffer_bytes));
    if (buffer == nullptr) throw std::bad_alloc();
    state.disk_io.file=state.file;
    state.io = avio_alloc_context(buffer, static_cast<int>(state.avio_buffer_bytes), 1, &state.disk_io, nullptr,
                                  write_bytes, seek_bytes);
    if (state.io == nullptr) {
      av_free(buffer);
      throw std::bad_alloc();
    }
    avcheck(avformat_alloc_output_context2(
                &state.context, nullptr,
                container == Container::Mp4 ? "mp4" : "matroska", nullptr),
            "allocate container");
    require(state.context != nullptr, "Container allocation returned no context");
    state.context->pb = state.io;
    state.context->flags |= AVFMT_FLAG_CUSTOM_IO;
    state.context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;

    for (const auto& descriptor : descriptors) {
      const auto kind = kind_index(descriptor.kind);
      require(kind >= 0 && state.stream_indices[static_cast<std::size_t>(kind)] < 0,
              "Duplicate or unknown stream descriptor");
      require(descriptor.time_base.numerator > 0 && descriptor.time_base.denominator > 0,
              "Stream time base must be positive");
      require(!descriptor.codec_extradata.empty() &&
                  descriptor.codec_extradata.size() <=
                      static_cast<std::size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE),
              "Stream codec extradata is missing or excessive");
      const bool video = descriptor.kind == StreamKind::Video;
      require(video ? descriptor.codec == "h264" && descriptor.width > 0 &&
                          descriptor.height > 0 && descriptor.width <= INT_MAX &&
                          descriptor.height <= INT_MAX
                    : descriptor.codec == "aac" && descriptor.sample_rate > 0 &&
                          descriptor.sample_rate <= INT_MAX && descriptor.channels > 0,
              "Stream codec parameters are invalid");

      auto* stream = avformat_new_stream(state.context, nullptr);
      if (stream == nullptr) throw std::bad_alloc();
      stream->id = static_cast<int>(state.context->nb_streams - 1);
      state.stream_indices[static_cast<std::size_t>(kind)] = stream->index;
      state.source_time_bases[static_cast<std::size_t>(kind)] =
          {descriptor.time_base.numerator, descriptor.time_base.denominator};
      auto* codec = stream->codecpar;
      codec->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
      codec->codec_id = video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC;
      codec->extradata = static_cast<std::uint8_t*>(
          av_mallocz(descriptor.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (codec->extradata == nullptr) throw std::bad_alloc();
      std::memcpy(codec->extradata, descriptor.codec_extradata.data(),
                  descriptor.codec_extradata.size());
      codec->extradata_size = static_cast<int>(descriptor.codec_extradata.size());
      if (video) {
        codec->width = static_cast<int>(descriptor.width);
        codec->height = static_cast<int>(descriptor.height);
        stream->time_base = {1, 90'000};
      } else {
        codec->sample_rate = static_cast<int>(descriptor.sample_rate);
        codec->frame_size = 1024;
        av_channel_layout_default(&codec->ch_layout,
                                  static_cast<int>(descriptor.channels));
        stream->time_base = {1, static_cast<int>(descriptor.sample_rate)};
      }
      const auto* title = descriptor.kind != StreamKind::Video || descriptor.title.empty()
          ? stream_title(descriptor.kind)
          : descriptor.title.c_str();
      // MP4 names live in hdlr; duplicate raw udta/name triggers a demux warning.
      if (container == Container::Mkv)
        avcheck(av_dict_set(&stream->metadata, "title", title, 0), "set stream title");
      avcheck(av_dict_set(&stream->metadata, "handler_name", title, 0),
              "set stream handler title");
    }

    AVDictionary* options = nullptr;
    if (container == Container::Mp4) {
      // Wait for initial packet timestamps so edit lists preserve AAC/video offsets.
      av_dict_set(&options, "movflags", "use_metadata_tags+empty_moov+delay_moov+default_base_moof+frag_keyframe", 0);
      av_dict_set(&options, "frag_duration", "2000000", 0);
    }
    const auto header = avformat_write_header(state.context, &options);
    av_dict_free(&options);
    avcheck(header, "write container header");
    state.output = av_packet_alloc();
    if (state.output == nullptr) throw std::bad_alloc();
    state.opened = true;
    return Result<void>::success();
  } catch (const Failure& error) {
    state.abort_locked();
    return state.failure(error);
  } catch (const std::exception& error) {
    state.abort_locked();
    return Result<void>::failure({"mux.exception", error.what(), {}});
  }
}

Result<void> FfmpegContinuousMuxer::write(const EncodedPacket& packet) {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  try {
    require(state.opened && state.context != nullptr, "No destination is open");
    const auto kind = kind_index(packet.stream);
    require(kind >= 0 && state.stream_indices[static_cast<std::size_t>(kind)] >= 0,
            "Packet stream has no descriptor");
    require(packet.payload && !packet.payload->empty() &&
                packet.payload->size() <= static_cast<std::size_t>(INT_MAX) &&
                packet.duration > 0,
            "Packet data is invalid");
    if (!state.origin.has_value()) state.origin = packet.dts;

    AVPacket* output = state.output;
    av_packet_unref(output);
    const auto release = [&] { av_packet_unref(output); };
    try {
      auto owner = std::make_unique<std::shared_ptr<const std::vector<std::byte>>>(packet.payload);
      auto* data = static_cast<std::uint8_t*>(av_malloc(packet.payload->size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (!data) throw std::bad_alloc();
      std::memset(data + packet.payload->size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
      output->buf = av_buffer_create(data, packet.payload->size() + AV_INPUT_BUFFER_PADDING_SIZE, release_packet_copy, owner.get(), 0);
      if (!output->buf) { av_free(data); throw std::bad_alloc(); }
      owner.release();
      output->data = data;
      output->size = static_cast<int>(packet.payload->size());
      std::memcpy(output->data, packet.payload->data(), packet.payload->size());
      output->stream_index = state.stream_indices[static_cast<std::size_t>(kind)];
      output->pts = packet.pts - *state.origin;
      output->dts = packet.dts - *state.origin;
      output->duration = packet.duration;
      output->pos = -1;
      if (packet.keyframe) output->flags |= AV_PKT_FLAG_KEY;
      av_packet_rescale_ts(
          output, state.source_time_bases[static_cast<std::size_t>(kind)],
          state.context->streams[output->stream_index]->time_base);
      avcheck(av_interleaved_write_frame(state.context, output), "write packet");
      avcheck(state.io->error, "write container IO");
      ++state.media_packets;
      release();
    } catch (...) {
      release();
      throw;
    }
    return Result<void>::success();
  } catch (const Failure& error) {
    return state.failure(error);
  } catch (const std::exception& error) {
    return Result<void>::failure({"mux.exception", error.what(), {}});
  }
}

Result<std::filesystem::path> FfmpegContinuousMuxer::finalize() {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  try {
    require(state.opened && state.context != nullptr, "No destination is open");
    state.trailer_attempted = true;
    avcheck(av_write_trailer(state.context), "write container trailer");
    avio_flush(state.io);
    avcheck(state.io->error, "flush container IO");
    if (!FlushFileBuffers(state.file)) {
      throw Failure{"mux.flush_failed", "Durable partial-file flush failed."};
    }
    const auto final = state.final_path;
    const auto partial = state.partial_path;
    state.close_context();
    if (!MoveFileExW(partial.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
      const auto system_error = GetLastError();
      throw Failure{
          system_error == ERROR_FILE_EXISTS || system_error == ERROR_ALREADY_EXISTS
              ? "mux.destination_exists"
              : "mux.publish_failed",
          "Atomic no-replace publication failed."};
    }
    state.owns_partial = false;
    state.final_path.clear();
    state.partial_path.clear();
    return Result<std::filesystem::path>::success(final);
  } catch (const Failure& error) {
    state.abort_locked();
    return Result<std::filesystem::path>::failure(
        {.code = error.code, .message = error.message, .hresult = std::nullopt});
  } catch (const std::exception& error) {
    state.abort_locked();
    return Result<std::filesystem::path>::failure(
        {"mux.exception", error.what(), {}});
  }
}

void FfmpegContinuousMuxer::abort() noexcept {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  state.abort_locked();
}

std::optional<std::filesystem::path> FfmpegContinuousMuxer::recovery_path() const {
  const auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  if (state.retained_path.empty()) return {};
  return state.retained_path;
}

}  // namespace rebelliocap
