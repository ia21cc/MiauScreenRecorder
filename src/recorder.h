#pragma once

#include "audio.h"
#include "common.h"
#include "encoder.h"
#include "screen_capture.h"

class Camera;

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

struct RecorderSettings {
  std::wstring outputDir;
  bool mp4 = true;            // true: graba en .mkv temporal y lo pasa a .mp4 al terminar
  int fps = 30;
  int quality = 1;            // 0 = baja, 1 = media, 2 = alta
  bool cursor = true;
  int cursorScalePct = 100;  // el cursor se dibuja en el video con este tamaño...
  int cursorRgb = -1;       // ...y este color (0xRRGGBB); -1 = el original de Windows
  bool separateTracks = false;  // escritorio y micrófono en pistas distintas
};

struct RecorderResult {
  bool ok = false;
  std::wstring path;
  std::string message;
};

// Coordina captura de pantalla, audio y codificación.
// El video se muestrea a ritmo constante con el reloj del sistema y el audio
// se rellena con silencio si un dispositivo no entrega datos, así las pistas
// siempre quedan sincronizadas.
class Recorder {
 public:
  ~Recorder() { Stop(); }

  // `desktop` y `mic` pueden ser nulos o no estar corriendo (se omiten).
  bool Start(const RECT& region, const RecorderSettings& s, AudioCapture* desktop, AudioCapture* mic,
             std::string& err);
  // Graba SOLO la cámara (con el micrófono, si hay). Usa los cuadros que ya entrega `cam` (la vista previa sigue activa).
  bool StartCamera(Camera* cam, const RecorderSettings& s, AudioCapture* mic, std::string& err);
  // Bloquea hasta terminar de guardar el archivo (puede tardar unos segundos con MP4).
  void Stop();

  bool Recording() const { return running_; }
  std::chrono::steady_clock::time_point StartTime() const { return t0_; }
  RecorderResult Result() const { return result_; }

 private:
  void VideoLoop();
  const uint8_t* GrabCamera();
  void AudioLoop();

  ScreenCapture cap_;
  Camera* cam_ = nullptr;  // != nullptr: el video viene de la cámara y no de la pantalla
  uint64_t camSerial_ = 0;
  int camSrcW_ = 0, camSrcH_ = 0, camW_ = 0, camH_ = 0;
  std::vector<uint8_t> camBuf_, camTmp_, camOut_;
  std::wstring prefix_ = L"Miau_";
  Encoder enc_;
  AudioCapture* desktop_ = nullptr;
  AudioCapture* mic_ = nullptr;

  std::thread videoTh_, audioTh_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::chrono::steady_clock::time_point t0_;

  int fps_ = 30;
  int audioTracks_ = 0;
  bool separate_ = false;
  bool mp4_ = false;
  bool hasDesktop_ = false, hasMic_ = false;
  std::wstring recPath_, finalPath_;
  RecorderResult result_;
};
