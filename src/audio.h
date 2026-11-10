#pragma once

#include "common.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioDevice {
  std::wstring id;   // ID de endpoint de WASAPI
  std::string name;  // nombre amigable (UTF-8)
};

// output = true  -> dispositivos de salida (se capturan con loopback)
// output = false -> dispositivos de entrada (micrófonos)
std::vector<AudioDevice> ListAudioDevices(bool output);

// Captura WASAPI en shared mode, convertida a float32 estéreo a 48 kHz.
// Sirve a la vez para los medidores de nivel y para la grabación.
class AudioCapture {
 public:
  AudioCapture() = default;
  ~AudioCapture() { Stop(); }
  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  // deviceId == L"default" usa el dispositivo predeterminado.
  // loopback = true captura lo que suena por un dispositivo de salida.
  void Start(const std::wstring& deviceId, bool loopback);
  void Stop();
  bool Running() const { return running_; }

  float PeakLeft() const { return peakL_; }
  float PeakRight() const { return peakR_; }

  // Mientras "recording" está activo, las muestras se guardan en un buffer
  // circular que el grabador lee con Read().
  void BeginRecording();
  void EndRecording();
  // Copia hasta `frames` frames estéreo intercalados. Devuelve los copiados.
  size_t Read(float* dst, size_t frames);

 private:
  void Run(std::wstring id, bool loopback);
  void Push(const float* interleaved, unsigned frames);

  std::thread th_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> recording_{false};
  std::atomic<float> peakL_{0.f};
  std::atomic<float> peakR_{0.f};

  std::mutex mu_;
  std::vector<float> ring_ = std::vector<float>(48000 * 2 * 4);  // 4 s
  size_t head_ = 0;
  size_t count_ = 0;  // en floats
};
