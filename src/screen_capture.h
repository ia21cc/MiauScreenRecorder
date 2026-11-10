#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// Captura una región del escritorio con DXGI Desktop Duplication.
// Entrega fotogramas BGRA (de arriba hacia abajo) del tamaño de la región.
// La región se recorta al monitor con mayor superficie en común.
class ScreenCapture {
 public:
  ScreenCapture() = default;
  ~ScreenCapture() { Shutdown(); }
  ScreenCapture(const ScreenCapture&) = delete;
  ScreenCapture& operator=(const ScreenCapture&) = delete;

  // `region` está en coordenadas del escritorio virtual (píxeles físicos).
  // Tamaño (%) y color (0xRRGGBB, -1 = original) con que se dibuja el cursor SOLO en el video.
  void SetCursorStyle(int scalePct, int rgb) {
    cursorScale_ = scalePct;
    cursorRgb_ = rgb;
  }
  bool Init(RECT region, bool drawCursor, std::string& err);
  void Shutdown();

  // Devuelve el fotograma más reciente (si no hubo cambios repite el anterior).
  // El puntero es válido hasta la siguiente llamada.
  const uint8_t* Grab();

  int Width() const { return w_; }
  int Height() const { return h_; }
  int Stride() const { return w_ * 4; }

 private:
  bool CreateDuplication(std::string& err);
  bool AcquireInto(UINT timeoutMs);
  void DrawStyledCursor(HCURSOR hc, const ICONINFO& ii, int hotX, int hotY);

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

  RECT region_{};   // región final en coordenadas del escritorio
  RECT local_{};    // región relativa al monitor
  int w_ = 0, h_ = 0;
  bool cursor_ = true;
  int cursorScale_ = 100, cursorRgb_ = -1;
  struct CursorImg {
    HCURSOR h = nullptr;
    int w = 0, hh = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> bgra;  // alfa recto
  } cur_;

  HDC memDC_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ oldBmp_ = nullptr;
  uint8_t* bits_ = nullptr;
};
