#include "encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

static std::string AvErr(int code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buf, sizeof(buf));
  return buf;
}

// Si un codificador (típicamente el de hardware de Windows) abre bien pero deja una cabecera inválida
// —pasaba al grabar por segunda vez sin cerrar la app—, se descarta y se prueba con el siguiente.
bool Encoder::Open(const EncoderConfig& cfg, std::string& err) {
  Close();
  skipCodecs_.clear();
  for (int attempt = 0; attempt < 6; attempt++) {
    bool headerFailed = false;
    if (OpenOnce(cfg, err, headerFailed)) return true;
    if (!headerFailed || videoName_.empty()) return false;
    skipCodecs_.push_back(videoName_);
  }
  return false;
}

bool Encoder::OpenOnce(const EncoderConfig& cfg, std::string& err, bool& headerFailed) {
  Close();
  videoName_.clear();
  cfg_ = cfg;
  const std::string path = WideToUtf8(cfg.path);

  int r = avformat_alloc_output_context2(&fmt_, nullptr, "matroska", path.c_str());
  if (r < 0 || !fmt_) {
    err = "No se pudo crear el contenedor: " + AvErr(r);
    Close();
    return false;
  }

  if (!OpenVideo(err)) {
    Close();
    return false;
  }

  audio_.resize((size_t)std::max(0, cfg.audioStreams));
  for (int i = 0; i < cfg.audioStreams; i++) {
    if (!OpenAudio(i, err)) {
      Close();
      return false;
    }
  }

  if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
    r = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) {
      err = "No se pudo crear el archivo de salida: " + AvErr(r);
      Close();
      return false;
    }
  }

  r = avformat_write_header(fmt_, nullptr);
  if (r < 0) {
    err = "No se pudo escribir la cabecera: " + AvErr(r);
    headerFailed = true;
    const std::string used = videoName_;
    Close();
    videoName_ = used;
    return false;
  }

  pkt_ = av_packet_alloc();
  opened_ = true;
  return true;
}

bool Encoder::OpenVideo(std::string& err) {
  struct Candidate {
    const char* name;
    AVPixelFormat pf;
  };
  // Primero hardware (NVIDIA, Intel, AMD), luego el codificador de Windows
  // (Media Foundation) y, como último recurso, MPEG-4 nativo de FFmpeg.
  static const Candidate candidates[] = {
      {"h264_nvenc", AV_PIX_FMT_NV12}, {"h264_qsv", AV_PIX_FMT_NV12}, {"h264_amf", AV_PIX_FMT_NV12},
      {"h264_mf", AV_PIX_FMT_NV12},    {"mpeg4", AV_PIX_FMT_YUV420P},
  };

  const bool globalHeader = (fmt_->oformat->flags & AVFMT_GLOBALHEADER) != 0;
  const int64_t bitrate = (int64_t)cfg_.videoBitrateKbps * 1000;

  for (const Candidate& cd : candidates) {
    if (std::find(skipCodecs_.begin(), skipCodecs_.end(), std::string(cd.name)) != skipCodecs_.end()) continue;
    const AVCodec* codec = avcodec_find_encoder_by_name(cd.name);
    if (!codec) continue;

    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c) continue;
    c->width = cfg_.width;
    c->height = cfg_.height;
    c->time_base = AVRational{1, cfg_.fps};
    c->framerate = AVRational{cfg_.fps, 1};
    c->pix_fmt = cd.pf;
    c->bit_rate = bitrate;
    c->rc_max_rate = bitrate * 3 / 2;
    c->rc_buffer_size = (int)std::min<int64_t>(bitrate, INT32_MAX);
    c->gop_size = cfg_.fps * 2;
    c->max_b_frames = 0;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    const std::string n = cd.name;
    if (n == "h264_nvenc") {
      av_dict_set(&opts, "preset", "p4", 0);
      av_dict_set(&opts, "rc", "vbr", 0);
    } else if (n == "h264_qsv") {
      av_dict_set(&opts, "preset", "veryfast", 0);
    } else if (n == "h264_amf") {
      av_dict_set(&opts, "usage", "transcoding", 0);
    } else if (n == "h264_mf") {
      av_dict_set(&opts, "hw_encoding", "1", 0);
    }

    const int r = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (r < 0) {
      avcodec_free_context(&c);
      continue;
    }

    vctx_ = c;
    pixfmt_ = (int)cd.pf;
    videoName_ = cd.name;
    break;
  }

  if (!vctx_) {
    err = "No se encontró ningún codificador de video que funcione en este equipo.";
    return false;
  }

  vst_ = avformat_new_stream(fmt_, nullptr);
  if (!vst_) {
    err = "No se pudo crear la pista de video.";
    return false;
  }
  avcodec_parameters_from_context(vst_->codecpar, vctx_);
  vst_->time_base = vctx_->time_base;
  vst_->avg_frame_rate = vctx_->framerate;

  vframe_ = av_frame_alloc();
  vframe_->format = pixfmt_;
  vframe_->width = cfg_.width;
  vframe_->height = cfg_.height;
  if (av_frame_get_buffer(vframe_, 0) < 0) {
    err = "No se pudo reservar memoria para los fotogramas.";
    return false;
  }

  sws_ = sws_getContext(cfg_.width, cfg_.height, AV_PIX_FMT_BGR0, cfg_.width, cfg_.height, (AVPixelFormat)pixfmt_,
                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_) {
    err = "No se pudo crear el conversor de color.";
    return false;
  }
  // RGB (rango completo) -> YUV BT.709 (rango limitado), igual que las etiquetas del códec.
  const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
  sws_setColorspaceDetails(sws_, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
  return true;
}

bool Encoder::OpenAudio(int index, std::string& err) {
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) {
    err = "FFmpeg no incluye el codificador AAC.";
    return false;
  }

  AudioTrack& a = audio_[(size_t)index];
  a.ctx = avcodec_alloc_context3(codec);
  a.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  a.ctx->sample_rate = cfg_.sampleRate;
  av_channel_layout_default(&a.ctx->ch_layout, 2);
  a.ctx->bit_rate = 192000;
  a.ctx->time_base = AVRational{1, cfg_.sampleRate};
  if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) a.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int r = avcodec_open2(a.ctx, codec, nullptr);
  if (r < 0) {
    err = "No se pudo abrir el codificador AAC: " + AvErr(r);
    return false;
  }

  a.st = avformat_new_stream(fmt_, nullptr);
  if (!a.st) {
    err = "No se pudo crear la pista de audio.";
    return false;
  }
  avcodec_parameters_from_context(a.st->codecpar, a.ctx);
  a.st->time_base = a.ctx->time_base;
  const char* title = "Audio";
  if (cfg_.audioStreams == 2) title = (index == 0) ? "Desktop audio" : "Microphone";
  av_dict_set(&a.st->metadata, "title", title, 0);

  a.frame = av_frame_alloc();
  a.frame->format = AV_SAMPLE_FMT_FLTP;
  a.frame->sample_rate = cfg_.sampleRate;
  a.frame->nb_samples = a.ctx->frame_size;
  av_channel_layout_copy(&a.frame->ch_layout, &a.ctx->ch_layout);
  if (av_frame_get_buffer(a.frame, 0) < 0) {
    err = "No se pudo reservar memoria para el audio.";
    return false;
  }
  return true;
}

void Encoder::Drain(AVCodecContext* c, AVStream* st) {
  while (avcodec_receive_packet(c, pkt_) == 0) {
    av_packet_rescale_ts(pkt_, c->time_base, st->time_base);
    pkt_->stream_index = st->index;
    av_interleaved_write_frame(fmt_, pkt_);  // toma posesión del contenido del paquete
  }
}

bool Encoder::WriteVideo(const uint8_t* bgra, int64_t pts) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!opened_) return false;

  if (av_frame_make_writable(vframe_) < 0) return false;
  const uint8_t* src[1] = {bgra};
  const int srcStride[1] = {cfg_.width * 4};
  sws_scale(sws_, src, srcStride, 0, cfg_.height, vframe_->data, vframe_->linesize);
  vframe_->pts = pts;

  if (avcodec_send_frame(vctx_, vframe_) < 0) return false;
  Drain(vctx_, vst_);
  return true;
}

bool Encoder::WriteAudio(int stream, const float* interleaved, int frames) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!opened_ || stream < 0 || stream >= (int)audio_.size()) return false;

  AudioTrack& a = audio_[(size_t)stream];
  a.fifo.insert(a.fifo.end(), interleaved, interleaved + (size_t)frames * 2);

  const int fs = a.ctx->frame_size;
  while ((int)(a.fifo.size() / 2) >= fs) {
    if (av_frame_make_writable(a.frame) < 0) return false;
    float* L = reinterpret_cast<float*>(a.frame->data[0]);
    float* R = reinterpret_cast<float*>(a.frame->data[1]);
    for (int i = 0; i < fs; i++) {
      L[i] = a.fifo[(size_t)i * 2];
      R[i] = a.fifo[(size_t)i * 2 + 1];
    }
    a.fifo.erase(a.fifo.begin(), a.fifo.begin() + (ptrdiff_t)fs * 2);
    a.frame->pts = a.nextPts;
    a.nextPts += fs;

    if (avcodec_send_frame(a.ctx, a.frame) < 0) return false;
    Drain(a.ctx, a.st);
  }
  return true;
}

void Encoder::Close() {
  std::lock_guard<std::mutex> lk(mu_);

  if (opened_) {
    if (vctx_) {
      avcodec_send_frame(vctx_, nullptr);
      Drain(vctx_, vst_);
    }
    for (AudioTrack& a : audio_) {
      if (a.ctx) {
        avcodec_send_frame(a.ctx, nullptr);
        Drain(a.ctx, a.st);
      }
    }
    av_write_trailer(fmt_);
  }
  opened_ = false;

  for (AudioTrack& a : audio_) {
    if (a.frame) av_frame_free(&a.frame);
    if (a.ctx) avcodec_free_context(&a.ctx);
  }
  audio_.clear();

  if (vframe_) av_frame_free(&vframe_);
  if (vctx_) avcodec_free_context(&vctx_);
  if (sws_) {
    sws_freeContext(sws_);
    sws_ = nullptr;
  }
  if (pkt_) av_packet_free(&pkt_);
  if (fmt_) {
    if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
  }
  vst_ = nullptr;
}

bool RemuxToMp4(const std::wstring& inPath, const std::wstring& outPath, std::string& err) {
  const std::string in = WideToUtf8(inPath);
  const std::string out = WideToUtf8(outPath);

  AVFormatContext* ic = nullptr;
  AVFormatContext* oc = nullptr;
  AVPacket* pkt = nullptr;
  std::vector<int> map;
  bool headerWritten = false;
  bool ok = false;

  auto cleanup = [&] {
    if (pkt) av_packet_free(&pkt);
    if (ic) avformat_close_input(&ic);
    if (oc) {
      if (oc->pb && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
      avformat_free_context(oc);
      oc = nullptr;
    }
  };

  int r = avformat_open_input(&ic, in.c_str(), nullptr, nullptr);
  if (r < 0) {
    err = "No se pudo abrir la grabación temporal: " + AvErr(r);
    cleanup();
    return false;
  }
  if ((r = avformat_find_stream_info(ic, nullptr)) < 0) {
    err = "No se pudo leer la grabación temporal: " + AvErr(r);
    cleanup();
    return false;
  }

  r = avformat_alloc_output_context2(&oc, nullptr, "mp4", out.c_str());
  if (r < 0 || !oc) {
    err = "No se pudo crear el MP4: " + AvErr(r);
    cleanup();
    return false;
  }

  map.assign(ic->nb_streams, -1);
  int next = 0;
  for (unsigned i = 0; i < ic->nb_streams; i++) {
    const AVMediaType t = ic->streams[i]->codecpar->codec_type;
    if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;
    AVStream* os = avformat_new_stream(oc, nullptr);
    if (!os || avcodec_parameters_copy(os->codecpar, ic->streams[i]->codecpar) < 0) {
      err = "No se pudieron copiar las pistas.";
      cleanup();
      return false;
    }
    os->codecpar->codec_tag = 0;
    av_dict_copy(&os->metadata, ic->streams[i]->metadata, 0);
    map[i] = next++;
  }

  if (!(oc->oformat->flags & AVFMT_NOFILE)) {
    if ((r = avio_open(&oc->pb, out.c_str(), AVIO_FLAG_WRITE)) < 0) {
      err = "No se pudo crear el MP4: " + AvErr(r);
      cleanup();
      return false;
    }
  }

  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "movflags", "+faststart", 0);
  r = avformat_write_header(oc, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    err = "No se pudo escribir la cabecera MP4: " + AvErr(r);
    cleanup();
    return false;
  }
  headerWritten = true;

  pkt = av_packet_alloc();
  while (av_read_frame(ic, pkt) >= 0) {
    const int idx = pkt->stream_index;
    if (idx < 0 || idx >= (int)map.size() || map[(size_t)idx] < 0) {
      av_packet_unref(pkt);
      continue;
    }
    AVStream* is = ic->streams[idx];
    AVStream* os = oc->streams[map[(size_t)idx]];
    av_packet_rescale_ts(pkt, is->time_base, os->time_base);
    pkt->stream_index = os->index;
    pkt->pos = -1;
    if (av_interleaved_write_frame(oc, pkt) < 0) break;
  }

  if (headerWritten) ok = av_write_trailer(oc) >= 0;
  if (!ok) err = "No se pudo finalizar el MP4.";
  cleanup();
  return ok;
}
