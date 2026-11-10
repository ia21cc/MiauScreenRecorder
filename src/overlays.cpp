#include "overlays.h"

#include "log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

const wchar_t* kCamClass = L"MiauCameraWindow";
const wchar_t* kStopClass = L"MiauStopWindow";
const wchar_t* kDotsClass = L"MiauDotsWindow";
const wchar_t* kMenuClass = L"MiauMenuWindow";
const wchar_t* kFrameClass = L"MiauFrameWindow";
const wchar_t* kLevelClass = L"MiauLevelWindow";

void EnsureClass(HINSTANCE hInst, const wchar_t* name, WNDPROC proc) {
  WNDCLASSEXW probe{};
  if (GetClassInfoExW(hInst, name, &probe)) return;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
  wc.lpszClassName = name;
  RegisterClassExW(&wc);
}

template <class T>
T* SelfFromCreate(HWND hwnd, UINT msg, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void ApplyRoundedRegion(HWND hwnd, int w, int h, int radius) {
  HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
  SetWindowRgn(hwnd, rgn, TRUE);  // el sistema se queda con la región
}

}  // namespace

// ───────────────────────────── CameraWindow ─────────────────────────────

bool CameraWindow::Create(HINSTANCE hInst, Camera* cam) {
  cam_ = cam;
  EnsureClass(hInst, kCamClass, &CameraWindow::Proc);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCamClass, L"Miau camera", WS_POPUP,
                          0, 0, 320, 180, nullptr, nullptr, hInst, this);
  return hwnd_ != nullptr;
}

void CameraWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void CameraWindow::Show(int x, int y, int width, float aspect) {
  if (!hwnd_) return;
  serial_ = 0;
  width_ = std::clamp(width, 120, 1920);
  aspect_ = aspect > 0.1f ? aspect : 16.f / 9.f;
  const int height = (int)(width_ / aspect_);
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ApplyRoundedRegion(hwnd_, width_, height, 28);
  SetTimer(hwnd_, 1, 33, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CameraWindow::Hide() {
  if (!hwnd_) return;
  KillTimer(hwnd_, 1);
  ShowWindow(hwnd_, SW_HIDE);  // solo se oculta: "Show camera" debe poder volver a mostrarla (Reshow)
}

void CameraWindow::Reshow() {
  if (!hwnd_) return;
  serial_ = 0;
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(hwnd_, 1, 33, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CameraWindow::ResizeAnchored(int width, bool anchorRight, bool anchorBottom) {
  if (!hwnd_) return;
  RECT wr;
  GetWindowRect(hwnd_, &wr);
  width_ = std::clamp(width, 140, std::max(140, (int)(GetSystemMetrics(SM_CXVIRTUALSCREEN) * 0.9)));
  const int height = (fw_ > 0 && fh_ > 0) ? width_ * fh_ / fw_ : (int)(width_ / aspect_);
  const int x = anchorRight ? wr.right - width_ : wr.left;
  const int y = anchorBottom ? wr.bottom - height : wr.top;
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height, SWP_NOACTIVATE);
  ApplyRoundedRegion(hwnd_, width_, height, 28);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CameraWindow::ResizeToWidth(int width) {
  RECT wr;
  GetWindowRect(hwnd_, &wr);
  const int maxW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  width_ = std::clamp(width, 120, maxW);
  const int height = (fw_ > 0 && fh_ > 0) ? width_ * fh_ / fw_ : (int)(width_ / aspect_);
  SetWindowPos(hwnd_, HWND_TOPMOST, wr.left, wr.top, width_, height, SWP_NOACTIVATE);
  ApplyRoundedRegion(hwnd_, width_, height, 28);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK CameraWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CameraWindow* self = SelfFromCreate<CameraWindow>(hwnd, msg, lp);
  if (self) return self->OnMessage(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CameraWindow::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      if (cam_ && cam_->GetFrame(px_, fw_, fh_, serial_)) {
        // Si la cámara es mucho más grande que la ventana (p. ej. Full HD), reducimos por promedio de bloques:
        // se ve mucho más nítido que dejar que Windows la achique de golpe.
        const int k = fw_ / std::max(1, width_ * 2);
        if (k >= 2) ReduceBox(px_.data(), fw_, fh_, k, small_, sw_, sh_);
        else small_.clear();
        // Ajusta la altura a la proporción real de la cámara.
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const int wantH = width_ * fh_ / std::max(fw_, 1);
        if (std::abs((wr.bottom - wr.top) - wantH) > 2) ResizeToWidth(width_);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      Paint(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));  // cruz de mover en toda la ventana
      return TRUE;
    case WM_LBUTTONDOWN: {
      down_ = true;
      GetCursorPos(&downCursor_);
      RECT r;
      GetWindowRect(hwnd, &r);
      downPos_ = POINT{r.left, r.top};
      SetCapture(hwnd);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (down_) {
        POINT c;
        GetCursorPos(&c);
        SetWindowPos(hwnd, HWND_TOPMOST, downPos_.x + c.x - downCursor_.x, downPos_.y + c.y - downCursor_.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
      }
      return 0;
    case WM_LBUTTONUP:
      down_ = false;
      if (GetCapture() == hwnd) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      down_ = false;
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wp);
      const double f = delta > 0 ? 1.08 : 1.0 / 1.08;
      ResizeToWidth((int)(width_ * f));
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void CameraWindow::Paint(HDC hdc, const RECT& rc) {
  const bool useSmall = !small_.empty() && sw_ > 0 && sh_ > 0;
  if (px_.empty() || fw_ <= 0 || fh_ <= 0) {
    HBRUSH br = CreateSolidBrush(RGB(13, 11, 43));
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    return;
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = useSmall ? sw_ : fw_;
  bi.bmiHeader.biHeight = useSmall ? -sh_ : -fh_;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(hdc, HALFTONE);
  SetBrushOrgEx(hdc, 0, 0, nullptr);
  StretchDIBits(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, useSmall ? sw_ : fw_, useSmall ? sh_ : fh_,
                useSmall ? small_.data() : px_.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// ────────────────────────────── StopWindow ──────────────────────────────

bool StopWindow::Create(HINSTANCE hInst, HWND notify) {
  notify_ = notify;
  scale_ = (float)GetDpiForSystem() / 96.0f;
  w_ = (int)(180 * scale_);
  h_ = (int)(52 * scale_);

  hInst_ = hInst;
  EnsureClass(hInst, kStopClass, &StopWindow::Proc);
  if (!MakeWindow()) return false;

  font_ = CreateFontW(-(int)(18 * scale_), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  fontSmall_ = CreateFontW(-(int)(10 * scale_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Segoe UI");
  return true;
}

// La ventana se crea de nuevo en cada grabación: al ocultar y volver a mostrar una ventana excluida de la captura,
// Windows la dejaba como un recuadro negro en el video (desde la 2.ª grabación).
bool StopWindow::MakeWindow() {
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kStopClass, L"Miau stop", WS_POPUP, 0,
                          0, w_, h_, nullptr, nullptr, hInst_, this);
  if (!hwnd_) return false;
  down_ = moved_ = false;
  SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);  // que el botón no salga en la grabación
  ApplyRoundedRegion(hwnd_, w_, h_, (int)(22 * scale_));
  return true;
}

void StopWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (fontSmall_) {
    DeleteObject(fontSmall_);
    fontSmall_ = nullptr;
  }
}

void StopWindow::Show(int x, int y, std::chrono::steady_clock::time_point start) {
  if (!hwnd_ && !MakeWindow()) return;
  start_ = start;
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(hwnd_, 1, 500, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
  KeepExcluded();
}

// Windows puede perder la exclusión de captura al ocultar/mostrar la ventana (pasaba desde la 2.ª grabación):
// se vuelve a pedir al mostrarla y se verifica en cada tick del cronómetro.
void StopWindow::KeepExcluded() {
  DWORD cur = 0;
  if (!GetWindowDisplayAffinity(hwnd_, &cur) || cur != WDA_EXCLUDEFROMCAPTURE)
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
}

void StopWindow::Hide() {
  if (!hwnd_) return;
  KillTimer(hwnd_, 1);
  ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK StopWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  StopWindow* self = SelfFromCreate<StopWindow>(hwnd, msg, lp);
  if (self) return self->OnMessage(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StopWindow::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      KeepExcluded();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      Paint(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR: {
      // Puntero normal solo sobre el cuadrado STOP; cruz de mover en el resto del cuadro.
      POINT c;
      GetCursorPos(&c);
      ScreenToClient(hwnd, &c);
      const bool onButton = c.x < (int)(56 * scale_) && !(down_ && moved_);
      SetCursor(LoadCursorW(nullptr, onButton ? IDC_ARROW : IDC_SIZEALL));
      return TRUE;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    // Se puede arrastrar desde cualquier punto. Un clic sin mover sobre el cuadrado STOP detiene la grabación.
    case WM_LBUTTONDOWN: {
      down_ = true;
      moved_ = false;
      downInButton_ = GET_X_LPARAM(lp) < (int)(56 * scale_);
      GetCursorPos(&downCursor_);
      RECT r;
      GetWindowRect(hwnd, &r);
      downPos_ = POINT{r.left, r.top};
      SetCapture(hwnd);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (down_) {
        POINT c;
        GetCursorPos(&c);
        const int dx = c.x - downCursor_.x, dy = c.y - downCursor_.y;
        if (moved_ || std::abs(dx) + std::abs(dy) > (int)(4 * scale_)) {
          moved_ = true;
          SetWindowPos(hwnd, HWND_TOPMOST, downPos_.x + dx, downPos_.y + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (down_) {
        down_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!moved_ && downInButton_) PostMessageW(notify_, WM_APP_STOP, 0, 0);
      }
      return 0;
    case WM_CAPTURECHANGED:
      down_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void StopWindow::Paint(HDC hdc, const RECT& rc) {
  const float s = scale_;

  HBRUSH bg = CreateSolidBrush(RGB(13, 11, 43));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  // Borde neón.
  HPEN pen = CreatePen(PS_SOLID, std::max(2, (int)(2 * s)), RGB(78, 234, 214));
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  const int r = (int)(22 * s);
  RoundRect(hdc, 1, 1, rc.right - 1, rc.bottom - 1, r, r);
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Cuadrado rosa de STOP.
  HBRUSH pink = CreateSolidBrush(RGB(209, 123, 240));
  HPEN none = CreatePen(PS_NULL, 0, 0);
  oldBr = SelectObject(hdc, pink);
  oldPen = SelectObject(hdc, none);
  const int qs = (int)(16 * s);
  const int qx = (int)(20 * s);
  const int qy = (int)(8 * s);
  RoundRect(hdc, qx, qy, qx + qs, qy + qs, (int)(4 * s), (int)(4 * s));
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBr);
  DeleteObject(none);
  DeleteObject(pink);

  SetBkMode(hdc, TRANSPARENT);

  // Texto "STOP" pequeño bajo el cuadrado.
  HGDIOBJ oldFont = SelectObject(hdc, fontSmall_);
  SetTextColor(hdc, RGB(200, 160, 235));
  RECT st = {0, (int)(27 * s), (int)(56 * s), rc.bottom - 2};
  DrawTextW(hdc, L"STOP", -1, &st, DT_CENTER | DT_TOP | DT_SINGLELINE);

  // "REC" + tiempo transcurrido.
  const auto el = std::chrono::steady_clock::now() - start_;
  const long long secs = std::chrono::duration_cast<std::chrono::seconds>(el).count();
  wchar_t time[32];
  if (secs >= 3600)
    swprintf(time, 32, L"%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
  else
    swprintf(time, 32, L"%02lld:%02lld", secs / 60, secs % 60);

  SelectObject(hdc, font_);
  SetTextColor(hdc, RGB(255, 80, 90));
  RECT rec = {(int)(62 * s), 0, (int)(104 * s), rc.bottom};
  DrawTextW(hdc, L"REC", -1, &rec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SetTextColor(hdc, RGB(120, 245, 255));
  RECT tm = {(int)(104 * s), 0, rc.right - (int)(8 * s), rc.bottom};
  DrawTextW(hdc, time, -1, &tm, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(hdc, oldFont);
}

// ──────────────────────────── CameraControls ────────────────────────────

bool CameraControls::Create(HINSTANCE hInst, CameraWindow* cam) {
  cam_ = cam;
  scale_ = (float)GetDpiForSystem() / 96.0f;
  dw_ = (int)(46 * scale_);
  dh_ = (int)(28 * scale_);
  mw_ = (int)(168 * scale_);
  mh_ = (int)(42 * scale_);

  EnsureClass(hInst, kDotsClass, &CameraControls::DotsProc);
  EnsureClass(hInst, kMenuClass, &CameraControls::MenuProc);
  EnsureClass(hInst, kFrameClass, &CameraControls::FrameProc);
  EnsureClass(hInst, kLevelClass, &CameraControls::LevelProc);

  const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
  dots_ = CreateWindowExW(ex, kDotsClass, L"Miau camera menu", WS_POPUP, 0, 0, dw_, dh_, nullptr, nullptr, hInst, this);
  menu_ = CreateWindowExW(ex, kMenuClass, L"Miau camera menu item", WS_POPUP, 0, 0, mw_, mh_, nullptr, nullptr, hInst,
                          this);
  level_ = CreateWindowExW(ex | WS_EX_TRANSPARENT, kLevelClass, L"Miau mic level", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                           hInst, this);
  frame_ = CreateWindowExW(ex & ~WS_EX_LAYERED, kFrameClass, L"Miau camera frame", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, this);
  if (!dots_ || !menu_ || !frame_ || !level_) return false;
  handleR_ = (int)(8 * scale_);
  pad_ = handleR_ + (int)(4 * scale_);

  // Importante: primero se activa la transparencia (capa) y DESPUÉS se pide excluir de la captura
  // (ver Exclude()); en el orden contrario Windows puede ignorar la exclusión y la ventana sale en el video.
  SetLayeredWindowAttributes(dots_, 0, 200, LWA_ALPHA);
  SetLayeredWindowAttributes(menu_, 0, 245, LWA_ALPHA);
  SetLayeredWindowAttributes(level_, 0, 215, LWA_ALPHA);
  ApplyRoundedRegion(dots_, dw_, dh_, dh_);
  ApplyRoundedRegion(menu_, mw_, mh_, (int)(18 * scale_));

  font_ = CreateFontW(-(int)(16 * scale_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  return true;
}

void CameraControls::Begin() {
  if (!dots_ || !cam_ || !cam_->Visible()) return;
  attached_ = true;
  hover_ = false;
  lastCam_ = RECT{};
  ApplyAlpha();
  Follow(true);
  hotUntil_ = 0;
  frameShown_ = false;
  levelShown_ = false;
  exDots_ = exMenu_ = exFrame_ = exLevel_ = false;
  lvl_ = 0.f;
  lastTick_ = GetTickCount64();
  SetTimer(dots_, 1, 16, nullptr);
}

void CameraControls::End() {
  if (!dots_) return;
  KillTimer(dots_, 1);
  HideFrame();
  HideLevel();
  resizing_ = false;
  CloseMenu();
  ShowWindow(dots_, SW_HIDE);
}

void CameraControls::Destroy() {
  if (dots_) {
    DestroyWindow(dots_);
    dots_ = nullptr;
  }
  if (menu_) {
    DestroyWindow(menu_);
    menu_ = nullptr;
  }
  if (frame_) {
    DestroyWindow(frame_);
    frame_ = nullptr;
  }
  if (level_) {
    DestroyWindow(level_);
    level_ = nullptr;
  }
  if (levelDc_) {
    if (levelOld_) SelectObject(levelDc_, levelOld_);
    DeleteDC(levelDc_);
    levelDc_ = nullptr;
  }
  if (levelBmp_) {
    DeleteObject(levelBmp_);
    levelBmp_ = nullptr;
  }
  if (frameDc_) {
    if (frameOld_) SelectObject(frameDc_, frameOld_);
    DeleteDC(frameDc_);
    frameDc_ = nullptr;
  }
  if (frameBmp_) {
    DeleteObject(frameBmp_);
    frameBmp_ = nullptr;
  }
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
}

void CameraControls::ApplyAlpha() {
  // Transparente cuando flota sola; más visible junto a la cámara o con el mouse encima.
  const BYTE a = hover_ ? 255 : (attached_ ? 200 : 110);
  SetLayeredWindowAttributes(dots_, 0, a, LWA_ALPHA);
  InvalidateRect(dots_, nullptr, FALSE);
}

// Coloca el botón en la esquina superior derecha de la cámara.
void CameraControls::Follow(bool force) {
  if (!attached_ || !cam_ || !cam_->Handle()) return;
  RECT r;
  GetWindowRect(cam_->Handle(), &r);
  const bool changed = !EqualRect(&r, &lastCam_);
  const bool reraise = (++tick_ % 10) == 0;  // mantenerlo por encima de la cámara
  if (!changed && !reraise && !force) return;
  lastCam_ = r;
  const int pad = (int)(14 * scale_);
  SetWindowPos(dots_, HWND_TOPMOST, r.right - dw_ - pad, r.top + pad, dw_, dh_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  Exclude(dots_, exDots_, "dots");
}

void CameraControls::ToggleCamera() {
  CloseMenu();
  if (attached_) {
    cam_->Hide();
    HideFrame();
    attached_ = false;
    ApplyAlpha();
  } else {
    cam_->Reshow();
    attached_ = true;
    lastCam_ = RECT{};
    ApplyAlpha();
    Follow(true);
  }
  InvalidateRect(dots_, nullptr, FALSE);
}

void CameraControls::OpenMenu() {
  RECT d;
  GetWindowRect(dots_, &d);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(dots_, MONITOR_DEFAULTTONEAREST), &mi);
  int x = d.right - mw_;
  int y = d.bottom + (int)(6 * scale_);
  if (y + mh_ > mi.rcWork.bottom) y = d.top - mh_ - (int)(6 * scale_);
  x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - mw_));
  menuHover_ = false;
  SetWindowPos(menu_, HWND_TOPMOST, x, y, mw_, mh_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  exMenu_ = false;
  Exclude(menu_, exMenu_, "menu");
  InvalidateRect(menu_, nullptr, FALSE);
  SetCapture(menu_);  // para detectar clics fuera del menú
}

void CameraControls::CloseMenu() {
  if (menu_ && IsWindowVisible(menu_)) {
    ShowWindow(menu_, SW_HIDE);
    if (GetCapture() == menu_) ReleaseCapture();
  }
}

LRESULT CALLBACK CameraControls::DotsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CameraControls* self = SelfFromCreate<CameraControls>(hwnd, msg, lp);
  if (self) return self->OnDots(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK CameraControls::MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CameraControls* self = SelfFromCreate<CameraControls>(hwnd, msg, lp);
  if (self) return self->OnMenu(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CameraControls::OnDots(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      Tick();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      PaintDots(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        tracking_ = true;
        hover_ = true;
        ApplyAlpha();
      }
      // Flotando sola se puede arrastrar; pegada a la cámara solo hace clic.
      if (down_ && !attached_) {
        POINT c;
        GetCursorPos(&c);
        const int dx = c.x - downCursor_.x, dy = c.y - downCursor_.y;
        if (moved_ || std::abs(dx) + std::abs(dy) > (int)(5 * scale_)) {
          moved_ = true;
          SetWindowPos(hwnd, HWND_TOPMOST, downPos_.x + dx, downPos_.y + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      hover_ = false;
      ApplyAlpha();
      return 0;
    case WM_LBUTTONDOWN: {
      if (menu_ && IsWindowVisible(menu_)) {  // segundo clic = cerrar el menú
        CloseMenu();
        return 0;
      }
      down_ = true;
      moved_ = false;
      GetCursorPos(&downCursor_);
      RECT r;
      GetWindowRect(hwnd, &r);
      downPos_ = POINT{r.left, r.top};
      SetCapture(hwnd);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (down_) {
        down_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!moved_) OpenMenu();
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void CameraControls::PaintDots(HDC hdc, const RECT& rc) {
  HBRUSH bg = CreateSolidBrush(RGB(13, 11, 43));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, std::max(1, (int)(1.5f * scale_)), RGB(78, 234, 214));
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, 1, 1, rc.right - 1, rc.bottom - 1, rc.bottom - 2, rc.bottom - 2);
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Tres puntitos.
  HBRUSH dot = CreateSolidBrush(hover_ ? RGB(120, 245, 255) : RGB(235, 240, 255));
  HPEN none = CreatePen(PS_NULL, 0, 0);
  oldBr = SelectObject(hdc, dot);
  oldPen = SelectObject(hdc, none);
  const int r = std::max(2, (int)(3 * scale_));
  const int cy = (rc.bottom - rc.top) / 2;
  const int cx = (rc.right - rc.left) / 2;
  const int gap = (int)(10 * scale_);
  for (int i = -1; i <= 1; i++) Ellipse(hdc, cx + i * gap - r, cy - r, cx + i * gap + r + 1, cy + r + 1);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBr);
  DeleteObject(none);
  DeleteObject(dot);
}

LRESULT CameraControls::OnMenu(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      PaintMenu(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const bool in = PtInRect(&rc, pt) != 0;
      if (in != menuHover_) {
        menuHover_ = in;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (PtInRect(&rc, pt)) ToggleCamera();
      else CloseMenu();  // clic fuera
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void CameraControls::PaintMenu(HDC hdc, const RECT& rc) {
  HBRUSH bg = CreateSolidBrush(menuHover_ ? RGB(40, 44, 120) : RGB(16, 14, 70));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, std::max(2, (int)(2 * scale_)), RGB(70, 200, 210));
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  const int r = (int)(18 * scale_);
  RoundRect(hdc, 1, 1, rc.right - 1, rc.bottom - 1, r, r);
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(222, 236, 255));
  HGDIOBJ oldFont = SelectObject(hdc, font_);
  RECT t = rc;
  DrawTextW(hdc, attached_ ? L"Hide camera" : L"Show camera", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(hdc, oldFont);
}

// ─────────────── Marco de esquinas para cambiar el tamaño ───────────────
// Es una ventana con transparencia por píxel: solo las 4 esquinas se pueden tocar, el resto deja pasar el mouse.
// Está excluida de la captura, así que las esquinas no salen en el video.

void CameraControls::Tick() {
  const ULONGLONG now = GetTickCount64();
  if (attached_ && cam_ && cam_->Visible()) {
    RECT r;
    GetWindowRect(cam_->Handle(), &r);
    POINT cp;
    GetCursorPos(&cp);
    RECT zone = r;
    InflateRect(&zone, pad_, pad_);
    if (PtInRect(&zone, cp) || resizing_) hotUntil_ = now + 700;  // visible mientras el cursor está encima
    if (now < hotUntil_) {
      const bool changed = !frameShown_ || frameW_ != (r.right - r.left) + 2 * pad_ ||
                           frameH_ != (r.bottom - r.top) + 2 * pad_ || !EqualRect(&r, &lastCam_);
      if (changed || (tick_ % 10) == 0) UpdateFrame(r, !frameShown_ || (tick_ % 10) == 0);
    } else if (frameShown_) {
      HideFrame();
    }
    // Barra de nivel del micrófono debajo de la cámara.
    if (mic_ && mic_->Running()) UpdateLevel(r);
    else HideLevel();
  } else {
    if (frameShown_) HideFrame();
    HideLevel();
  }
  Follow(false);  // el botón de 3 puntos queda siempre encima del marco

  // Reintenta la exclusión de la captura cada ~0,5 s en las ventanas donde aún no se logró.
  if ((++exTick_ % 30) == 0) {
    if (dots_ && IsWindowVisible(dots_)) Exclude(dots_, exDots_, "dots");
    if (menu_ && IsWindowVisible(menu_)) Exclude(menu_, exMenu_, "menu");
    if (frame_ && frameShown_) Exclude(frame_, exFrame_, "frame");
    if (level_ && levelShown_) Exclude(level_, exLevel_, "level");
  }
}

// Pide que la ventana NO aparezca en ninguna captura de pantalla (solo se ve en el monitor) y comprueba el resultado.
void CameraControls::Exclude(HWND h, bool& flag, const char* tag) {
  if (flag || !h) return;
  const BOOL ok = SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
  const DWORD err = ok ? 0 : GetLastError();
  DWORD cur = 0;
  GetWindowDisplayAffinity(h, &cur);
  flag = ok && cur == WDA_EXCLUDEFROMCAPTURE;
  LogLine("Excluir de la captura (%s): ok=%d error=%lu lectura=0x%lx -> %s", tag, (int)ok, err, cur,
          flag ? "OK" : "FALLO");
}

void CameraControls::HideLevel() {
  if (level_ && levelShown_) ShowWindow(level_, SW_HIDE);
  levelShown_ = false;
  exLevel_ = false;
}

LRESULT CALLBACK CameraControls::LevelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CameraControls* self = SelfFromCreate<CameraControls>(hwnd, msg, lp);
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;  // el mouse la atraviesa
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(40, 38, 84));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    if (self->levelFill_ > 0) {
      RECT f{0, 0, std::min((int)rc.right, self->levelFill_), rc.bottom};
      HBRUSH fg = CreateSolidBrush(RGB(60, 227, 180));
      FillRect(hdc, &f, fg);
      DeleteObject(fg);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// Barra semitransparente: crece al hablar y se reduce en silencio. Ventana normal (no por-píxel) excluida de la captura.
void CameraControls::UpdateLevel(const RECT& cam) {
  if (!level_) return;
  const ULONGLONG now = GetTickCount64();
  const float dt = std::min(0.1f, (float)(now - lastTick_) / 1000.f);
  lastTick_ = now;
  const float peak = std::max(mic_->PeakLeft(), mic_->PeakRight());
  const float db = peak > 1e-5f ? 20.f * std::log10(peak) : -100.f;
  const float n = std::clamp((db + 54.f) / 48.f, 0.f, 1.f);  // -54 dB .. -6 dB
  lvl_ = std::max(n, lvl_ - dt * 1.6f);                      // sube al instante, baja suave

  const int w = cam.right - cam.left;
  const int h = std::max(8, (int)(12 * scale_));
  if (w < 40) return;
  const int gap = (int)(8 * scale_);
  int x = cam.left, y = cam.bottom + gap;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromRect(&cam, MONITOR_DEFAULTTONEAREST), &mi);
  if (y + h > mi.rcMonitor.bottom) y = cam.top - gap - h;  // si no cabe abajo, va arriba

  const int fill = lvl_ < 0.02f ? 0 : std::max(h, (int)(lvl_ * w + 0.5f));
  const bool sizeChanged = w != levelW_ || h != levelH_;
  const bool moved = !levelShown_ || x != levelX_ || y != levelY_;
  if (!sizeChanged && !moved && fill == levelFill_) return;

  if (sizeChanged) {
    levelW_ = w;
    levelH_ = h;
    ApplyRoundedRegion(level_, w, h, h);
  }
  const bool repaint = sizeChanged || fill != levelFill_;
  levelFill_ = fill;
  SetWindowPos(level_, HWND_TOPMOST, x, y, w, h,
               SWP_NOACTIVATE | (levelShown_ ? 0 : SWP_SHOWWINDOW) | ((sizeChanged || moved) ? 0 : SWP_NOMOVE | SWP_NOSIZE));
  if (repaint) InvalidateRect(level_, nullptr, FALSE);
  if (!levelShown_) {
    levelShown_ = true;
    exLevel_ = false;
  }
  Exclude(level_, exLevel_, "level");
  levelX_ = x;
  levelY_ = y;
}

void CameraControls::HideFrame() {
  if (frame_ && frameShown_) ShowWindow(frame_, SW_HIDE);
  frameShown_ = false;
  exFrame_ = false;
}

// Dibuja los 4 círculos de las esquinas en el bitmap (alfa premultiplicado).
void CameraControls::DrawHandles(int w, int h) {
  std::fill(frameBits_, frameBits_ + (size_t)w * h, 0u);
  const float R = (float)handleR_;
  const int cxs[2] = {pad_, w - pad_};
  const int cys[2] = {pad_, h - pad_};
  for (int cy : cys) {
    for (int cx : cxs) {
      for (int y = cy - handleR_ - 1; y <= cy + handleR_ + 1; y++) {
        for (int x = cx - handleR_ - 1; x <= cx + handleR_ + 1; x++) {
          if (x < 0 || y < 0 || x >= w || y >= h) continue;
          const float d = std::sqrt((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
          const float cov = std::clamp(R + 0.5f - d, 0.f, 1.f);
          if (cov <= 0.f) continue;
          const bool ring = d > R - 2.5f * scale_;
          const int cr = ring ? 94 : 13, cg = ring ? 242 : 11, cb = ring ? 236 : 43;
          const int a = (int)(cov * 255.f);
          frameBits_[(size_t)y * w + x] = ((uint32_t)a << 24) | ((uint32_t)(cr * a / 255) << 16) |
                                          ((uint32_t)(cg * a / 255) << 8) | (uint32_t)(cb * a / 255);
        }
      }
    }
  }
}

void CameraControls::UpdateFrame(const RECT& cam, bool raise) {
  if (!frame_) return;
  const int w = (cam.right - cam.left) + 2 * pad_;
  const int h = (cam.bottom - cam.top) + 2 * pad_;
  if (w <= 0 || h <= 0) return;

  // Ventana normal (sin capa por píxel) con forma de 4 círculos: así Windows sí respeta la exclusión de la captura.
  const bool sizeChanged = w != frameW_ || h != frameH_;
  if (sizeChanged) {
    frameW_ = w;
    frameH_ = h;
    HRGN all = CreateRectRgn(0, 0, 0, 0);
    const int cxs[2] = {pad_, w - pad_};
    const int cys[2] = {pad_, h - pad_};
    for (int cy : cys) {
      for (int cx : cxs) {
        HRGN c = CreateEllipticRgn(cx - handleR_, cy - handleR_, cx + handleR_ + 1, cy + handleR_ + 1);
        CombineRgn(all, all, c, RGN_OR);
        DeleteObject(c);
      }
    }
    SetWindowRgn(frame_, all, FALSE);
  }
  SetWindowPos(frame_, HWND_TOPMOST, cam.left - pad_, cam.top - pad_, w, h,
               SWP_NOACTIVATE | ((raise || !frameShown_) ? SWP_SHOWWINDOW : 0));
  if (sizeChanged) InvalidateRect(frame_, nullptr, FALSE);
  frameShown_ = true;
  Exclude(frame_, exFrame_, "frame");
  lastCam_ = cam;
}

LRESULT CALLBACK CameraControls::FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CameraControls* self = SelfFromCreate<CameraControls>(hwnd, msg, lp);
  if (self) return self->OnFrame(hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CameraControls::OnFrame(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto cornerAt = [&](POINT screenPt) {
    RECT r;
    GetWindowRect(hwnd, &r);
    const int right = screenPt.x > (r.left + r.right) / 2 ? 1 : 0;
    const int bottom = screenPt.y > (r.top + r.bottom) / 2 ? 2 : 0;
    return right | bottom;
  };
  switch (msg) {
    case WM_SETCURSOR: {
      POINT c;
      GetCursorPos(&c);
      const int k = resizing_ ? corner_ : cornerAt(c);
      // Esquinas TL y BR: diagonal \ ; TR y BL: diagonal /
      SetCursor(LoadCursorW(nullptr, (k == 0 || k == 3) ? IDC_SIZENWSE : IDC_SIZENESW));
      return TRUE;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      HBRUSH fill = CreateSolidBrush(RGB(13, 11, 43));
      HPEN pen = CreatePen(PS_SOLID, std::max(2, (int)(2.5f * scale_)), RGB(94, 242, 236));
      HGDIOBJ oldB = SelectObject(hdc, fill), oldP = SelectObject(hdc, pen);
      const int cxs[2] = {pad_, (int)rc.right - pad_};
      const int cys[2] = {pad_, (int)rc.bottom - pad_};
      for (int cy : cys)
        for (int cx : cxs) Ellipse(hdc, cx - handleR_, cy - handleR_, cx + handleR_ + 1, cy + handleR_ + 1);
      SelectObject(hdc, oldB);
      SelectObject(hdc, oldP);
      DeleteObject(fill);
      DeleteObject(pen);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT c;
      GetCursorPos(&c);
      corner_ = cornerAt(c);
      resizing_ = true;
      startCursor_ = c;
      GetWindowRect(cam_->Handle(), &startRect_);
      SetCapture(hwnd);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (resizing_) {
        POINT c;
        GetCursorPos(&c);
        const float aspect = (float)(startRect_.right - startRect_.left) / (float)std::max(1, (int)(startRect_.bottom - startRect_.top));
        const float sx = (corner_ & 1) ? 1.f : -1.f, sy = (corner_ & 2) ? 1.f : -1.f;
        const float dx = sx * (float)(c.x - startCursor_.x);
        const float dy = sy * (float)(c.y - startCursor_.y) * aspect;  // cuánto crecería el ancho siguiendo la altura
        const float dw = std::fabs(dx) > std::fabs(dy) ? dx : dy;
        const int startW = startRect_.right - startRect_.left;
        // El borde opuesto a la esquina que se arrastra queda fijo.
        cam_->ResizeAnchored(startW + (int)dw, !(corner_ & 1), !(corner_ & 2));
        RECT r;
        GetWindowRect(cam_->Handle(), &r);
        UpdateFrame(r, false);
        Follow(true);
      }
      return 0;
    case WM_LBUTTONUP:
      resizing_ = false;
      if (GetCapture() == hwnd) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      resizing_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
