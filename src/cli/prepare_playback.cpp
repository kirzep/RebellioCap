#include "cli/prepare_playback.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
std::string utf8(const std::filesystem::path& path) {
  auto value = path.u8string();
  return std::string(value.begin(), value.end());
}
void check(int result) { if (result < 0) throw std::runtime_error("clips.playback_remux_failed"); }
struct Output {
  AVFormatContext* context{};
  int source{};
  ~Output() { if (context) { if (context->pb) avio_closep(&context->pb); avformat_free_context(context); } }
};
}

int prepare_playback(const std::filesystem::path& source, const std::filesystem::path& destination) {
  AVFormatContext* input = nullptr;
  try {
    check(avformat_open_input(&input, utf8(source).c_str(), nullptr, nullptr));
    check(avformat_find_stream_info(input, nullptr));
    std::vector<std::unique_ptr<Output>> outputs;
    nlohmann::json result = {{"video", ""}, {"tracks", nlohmann::json::array()}};
    bool video_found = false;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
      auto* stream = input->streams[i];
      const bool video = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
      if (!video && stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      if (video && video_found) continue;
      if (video) video_found = true;
      const auto file = video ? "video.mp4" : "audio-" + std::to_string(i) + ".m4a";
      auto output = std::make_unique<Output>();
      output->source = static_cast<int>(i);
      check(avformat_alloc_output_context2(&output->context, nullptr, "mp4", utf8(destination / file).c_str()));
      output->context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
      auto* target = avformat_new_stream(output->context, nullptr);
      if (!target) throw std::runtime_error("clips.playback_stream_failed");
      check(avcodec_parameters_copy(target->codecpar, stream->codecpar));
      target->codecpar->codec_tag = 0;
      if (video && target->codecpar->codec_id == AV_CODEC_ID_HEVC) target->codecpar->codec_tag = MKTAG('h','v','c','1');
      target->time_base = stream->time_base;
      check(avio_open(&output->context->pb, utf8(destination / file).c_str(), AVIO_FLAG_WRITE));
      check(avformat_write_header(output->context, nullptr));
      if (video) result["video"] = file;
      else {
        const auto* title = av_dict_get(stream->metadata, "title", nullptr, 0);
        if (!title) title = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
        std::string label = title ? title->value : "Аудиодорожка";
        if (label == "Mixed") label = "Системный звук + микрофон";
        if (label == "System") label = "Системный звук";
        if (label == "Microphone") label = "Микрофон";
        result["tracks"].push_back({{"path", file}, {"label", label}});
      }
      outputs.push_back(std::move(output));
    }
    if (!video_found) throw std::runtime_error("clips.playback_video_missing");
    AVPacket* packet = av_packet_alloc();
    if (!packet) throw std::bad_alloc();
    int read_result = 0;
    try {
      while ((read_result = av_read_frame(input, packet)) >= 0) {
        for (const auto& output : outputs) {
          if (packet->stream_index != output->source) continue;
          const auto origin = input->start_time == AV_NOPTS_VALUE ? 0 : av_rescale_q(input->start_time, AV_TIME_BASE_Q, input->streams[output->source]->time_base);
          if (packet->pts != AV_NOPTS_VALUE) packet->pts -= origin;
          if (packet->dts != AV_NOPTS_VALUE) packet->dts -= origin;
          av_packet_rescale_ts(packet, input->streams[output->source]->time_base, output->context->streams[0]->time_base);
          packet->stream_index = 0;
          packet->pos = -1;
          check(av_interleaved_write_frame(output->context, packet));
          break;
        }
        av_packet_unref(packet);
      }
      if (read_result != AVERROR_EOF) check(read_result);
      for (const auto& output : outputs) check(av_write_trailer(output->context));
    } catch (...) { av_packet_free(&packet); throw; }
    av_packet_free(&packet);
    avformat_close_input(&input);
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    avformat_close_input(&input);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
