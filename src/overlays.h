#pragma once

#include "audio.h"
#include "camera.h"
#include "common.h"

#include <chrono>
#include <cstdint>
#include <vector>

// Ventana flotante con la cámara. Está siempre encima y se puede arrastrar a
// cualquier lugar de la pantalla (también mientras se graba). Como forma parte
// del escritorio, aparece en el video si queda dentro del área grabada.
// Con la rueda del mouse se cambia su tamaño.
class CameraWindow {
 public:
  ~CameraWindow() { Destroy(); }

  bool Create(HINSTANCE hInst, Camera* cam);
  // x, y: esquina superior izquierda en coordenadas del escritorio virtual.
  // aspect = ancho / alto de la cámara (para calcular la altura desde el inicio).
  void Show(int x, int y, int width, float aspect = 16.f / 9.f);
  void Hide();
  void Reshow();  // vuelve a mostrarla donde estaba
  void Destroy();
  HWND Handle() const { return hwnd_; }
  bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
  int Width() const { return width_; }
  // Cambia el ancho conservando la proporción y dejando fijo el borde derecho/inferior indicado.
  void ResizeAnchored(int width, bool anchorRight, bool anchorBottom);

 private:
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT OnMessage(HWND, UINT, WPARAM, LPARAM);
  void Paint(HDC hdc, const RECT& rc);
  void ResizeToWidth(int width);

  HWND hwnd_ = nullptr;
  Camera* cam_ = nullptr;
  std::vector<uint8_t> px_;
  std::vector<uint8_t> small_;  // versión reducida (promedio de bloques) para dibujar nítido
  int sw_ = 0, sh_ = 0;
  int fw_ = 0, fh_ = 0;
  uint64_t serial_ = 0;
  int width_ = 320;
  float aspect_ = 16.f / 9.f;
  bool down_ = false;  // arrastre manual
  POINT downCursor_{}, downPos_{};
};

// Botón flotante "STOP" con el tiempo de grabación. Se puede arrastrar y NO
// aparece en el video (se excluye de la captura, Windows 10 2004 o superior).
class StopWindow {
 public:
  ~StopWindow() { Destroy(); }

  bool Create(HINSTANCE hInst, HWND notify);
  void Show(int x, int y, std::chrono::steady_clock::time_point start);
  void Hide();
  void Destroy();
  int Width() const { return w_; }
  int Height() const { return h_; }

 private:
  void KeepExcluded();
  bool MakeWindow();
  HINSTANCE hInst_ = nullptr;
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT OnMessage(HWND, UINT, WPARAM, LPARAM);
  void Paint(HDC hdc, const RECT& rc);

  HWND hwnd_ = nullptr;
  HWND notify_ = nullptr;
  HFONT font_ = nullptr;
  HFONT fontSmall_ = nullptr;
  std::chrono::steady_clock::time_point start_;
  int w_ = 180, h_ = 52;
  float scale_ = 1.f;
  bool down_ = false, moved_ = false, downInButton_ = false;  // arrastre manual
  POINT downCursor_{}, downPos_{};
};

// Botón de 3 puntitos de la cámara. Nunca aparece en el video (se excluye de la captura).
//  - Con la cámara visible: queda en su esquina superior derecha y la sigue al arrastrarla.
//    Al hacer clic muestra "Hide camera".
//  - Con la cámara oculta: queda como botón flotante transparente que también se arrastra.
//    Al hacer clic muestra "Show camera".
class CameraControls {
 public:
  ~CameraControls() { Destroy(); }

  bool Create(HINSTANCE hInst, CameraWindow* cam);
  void SetMic(AudioCapture* mic) { mic_ = mic; }  // para la barra de nivel (nullptr = sin barra)
  void Begin();  // al empezar a grabar (si la cámara está visible)
  void End();    // al terminar
  void Destroy();

 private:
  static LRESULT CALLBACK DotsProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK MenuProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK LevelProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT OnFrame(HWND, UINT, WPARAM, LPARAM);
  void Tick();
  void UpdateFrame(const RECT& camRect, bool raise);
  void HideFrame();
  void DrawHandles(int w, int h);
  void UpdateLevel(const RECT& camRect);
  void HideLevel();
  void Exclude(HWND h, bool& flag, const char* tag);
  LRESULT OnDots(HWND, UINT, WPARAM, LPARAM);
  LRESULT OnMenu(HWND, UINT, WPARAM, LPARAM);
  void PaintDots(HDC hdc, const RECT& rc);
  void PaintMenu(HDC hdc, const RECT& rc);
  void Follow(bool force);
  void OpenMenu();
  void CloseMenu();
  void ApplyAlpha();
  void ToggleCamera();

  HWND dots_ = nullptr;
  HWND menu_ = nullptr;
  HWND frame_ = nullptr;  // marco con 4 esquinas para cambiar el tamaño (no sale en el video)
  HDC frameDc_ = nullptr;
  HBITMAP frameBmp_ = nullptr;
  HGDIOBJ frameOld_ = nullptr;
  uint32_t* frameBits_ = nullptr;
  int frameW_ = 0, frameH_ = 0, pad_ = 12, handleR_ = 8;
  bool frameShown_ = false;
  ULONGLONG hotUntil_ = 0;
  bool resizing_ = false;
  int corner_ = 0;  // bit0 = derecha, bit1 = abajo
  RECT startRect_{};
  POINT startCursor_{};
  // Barra de nivel del micrófono bajo la cámara (semitransparente, no se puede tocar ni sale en el video).
  HWND level_ = nullptr;
  HDC levelDc_ = nullptr;
  HBITMAP levelBmp_ = nullptr;
  HGDIOBJ levelOld_ = nullptr;
  uint32_t* levelBits_ = nullptr;
  int levelW_ = 0, levelH_ = 0, levelFill_ = -1;
  bool levelShown_ = false;
  float lvl_ = 0.f;
  ULONGLONG lastTick_ = 0;
  AudioCapture* mic_ = nullptr;
  // Ya se aplicó "excluir de la captura" a cada ventana (se reintenta hasta lograrlo).
  bool exDots_ = false, exMenu_ = false, exFrame_ = false, exLevel_ = false;
  int exTick_ = 0;
  int levelX_ = 0, levelY_ = 0;
  CameraWindow* cam_ = nullptr;
  HFONT font_ = nullptr;
  float scale_ = 1.f;
  int dw_ = 46, dh_ = 28, mw_ = 160, mh_ = 40;
  bool attached_ = true;   // true = cámara visible
  bool hover_ = false, menuHover_ = false, tracking_ = false;
  bool down_ = false, moved_ = false;
  POINT downCursor_{}, downPos_{};
  RECT lastCam_{};
  int tick_ = 0;
};
