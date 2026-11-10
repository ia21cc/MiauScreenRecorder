#pragma once

#include "common.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

struct CameraInfo {
  std::wstring symlink;  // enlace simbólico del dispositivo (identificador)
  std::string name;      // nombre amigable (UTF-8)
};

// Requiere MFStartup() previo.
std::vector<CameraInfo> ListCameras();

// Toma UNA foto con la mayor resolución que ofrezca la cámara (puede ser mayor que la de la vista previa).
// La cámara no debe estar abierta por otro lector (detén `Camera` antes). Bloquea unos segundos: usar en un hilo aparte.
bool CaptureStill(const std::wstring& symlink, std::vector<uint8_t>& bgra, int& w, int& h, std::string& err);

// Lee la cámara con Media Foundation y deja disponible el último fotograma
// en BGRA (alpha = 255), de arriba hacia abajo.
class Camera {
 public:
  Camera() = default;
  ~Camera() { Stop(); }
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;

  void Start(const std::wstring& symlink);
  void Stop();
  bool Running() const { return running_; }
  // Diagnóstico: intento actual, modo y paso en el que está (para mostrarlo en la interfaz).
  int Attempt() const { return attempt_; }
  int Mode() const { return mode_; }
  const char* Stage() const { return stage_; }
  long LastHr() const { return lastHr_; }

  // Copia el último fotograma si es más nuevo que `serial` (que se actualiza).
  // Cada consumidor lleva su propio contador.
  bool GetFrame(std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& serial);
  // Copia el último cuadro recibido (sin importar si ya se leyó). Devuelve false si todavía no hay ninguno.
  bool Snapshot(std::vector<uint8_t>& bgra, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mu_);
    if (frame_.empty()) return false;
    bgra = frame_;
    w = w_;
    h = h_;
    return true;
  }

 private:
  std::atomic<int> attempt_{0}, mode_{0};
  std::atomic<const char*> stage_{""};
  std::atomic<long> lastHr_{0};
  void Run(std::wstring link);

  std::thread th_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};

  std::mutex mu_;
  std::vector<uint8_t> frame_;
  int w_ = 0, h_ = 0;
  uint64_t serial_ = 0;
};

// Reduce una imagen BGRA promediando bloques de k×k píxeles (mucho más nítido que submuestrear).
// El resultado mide (w/k)×(h/k) con alfa opaco.
inline void ReduceBox(const uint8_t* src, int w, int h, int k, std::vector<uint8_t>& dst, int& dw, int& dh) {
  if (k < 2) k = 1;
  dw = w / k;
  dh = h / k;
  dst.resize((size_t)dw * dh * 4);
  std::vector<uint32_t> acc((size_t)dw * 3);
  const uint32_t div = (uint32_t)(k * k);
  for (int y = 0; y < dh; y++) {
    std::fill(acc.begin(), acc.end(), 0u);
    for (int j = 0; j < k; j++) {
      const uint8_t* row = src + ((size_t)(y * k + j) * w) * 4;
      for (int x = 0; x < dw; x++) {
        const uint8_t* p = row + (size_t)x * k * 4;
        uint32_t b = 0, g = 0, r = 0;
        for (int i = 0; i < k; i++, p += 4) {
          b += p[0];
          g += p[1];
          r += p[2];
        }
        acc[(size_t)x * 3] += b;
        acc[(size_t)x * 3 + 1] += g;
        acc[(size_t)x * 3 + 2] += r;
      }
    }
    uint8_t* o = dst.data() + (size_t)y * dw * 4;
    for (int x = 0; x < dw; x++) {
      o[x * 4] = (uint8_t)(acc[(size_t)x * 3] / div);
      o[x * 4 + 1] = (uint8_t)(acc[(size_t)x * 3 + 1] / div);
      o[x * 4 + 2] = (uint8_t)(acc[(size_t)x * 3 + 2] / div);
      o[x * 4 + 3] = 255;
    }
  }
}
