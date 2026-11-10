#pragma once

#include "common.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

struct EncoderConfig {
  std::wstring path;
  int width = 0;
  int height = 0;
  int fps = 30;
  int videoBitrateKbps = 8000;
  int audioStreams = 0;  // 0, 1 o 2 pistas AAC
  int sampleRate = 48000;
};

// Codifica video BGRA (H.264 por hardware o Media Foundation) y audio AAC,
// y los guarda en Matroska con las bibliotecas de FFmpeg.
class Encoder {
 public:
  Encoder() = default;
  ~Encoder() { Close(); }
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool Open(const EncoderConfig& cfg, std::string& err);
  // `pts` está en unidades de 1/fps. `bgra` tiene width*height*4 bytes.
  bool WriteVideo(const uint8_t* bgra, int64_t pts);
  // Audio float32 estéreo intercalado a 48 kHz para la pista `stream`.
  bool WriteAudio(int stream, const float* interleaved, int frames);
  void Close();

  const std::string& VideoEncoderName() const { return videoName_; }

 private:
  struct AudioTrack {
    AVCodecContext* ctx = nullptr;
    AVStream* st = nullptr;
    AVFrame* frame = nullptr;
    std::vector<float> fifo;  // estéreo intercalado
    int64_t nextPts = 0;
  };

  bool OpenOnce(const EncoderConfig& cfg, std::string& err, bool& headerFailed);
  bool OpenVideo(std::string& err);
  std::vector<std::string> skipCodecs_;  // codificadores que abrieron pero produjeron una cabecera inválida
  bool OpenAudio(int index, std::string& err);
  void Drain(AVCodecContext* c, AVStream* st);

  EncoderConfig cfg_;
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* vctx_ = nullptr;
  AVStream* vst_ = nullptr;
  AVFrame* vframe_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVPacket* pkt_ = nullptr;
  std::vector<AudioTrack> audio_;
  std::string videoName_;
  int pixfmt_ = 0;
  bool opened_ = false;
  std::mutex mu_;
};

// Copia las pistas de un .mkv a un .mp4 sin recodificar.
bool RemuxToMp4(const std::wstring& inPath, const std::wstring& outPath, std::string& err);
