#include "region_selector.h"

#include "cursor_style.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

HCURSOR g_selCursor = nullptr;  // cursor con el tamaño/color elegidos (nullptr = cruz normal)

struct SelState {
  int vx = 0, vy = 0, vw = 0, vh = 0;  // escritorio virtual

  HDC dcOrig = nullptr, dcDim = nullptr;
  HBITMAP bmpOrig = nullptr, bmpDim = nullptr;
  HGDIOBJ oldOrig = nullptr, oldDim = nullptr;

  bool dragging = false;
  bool hasRect = false;
  bool done = false;
  bool ok = false;
  POINT a{}, b{};
  RECT result{};  // coordenadas locales a la capa
};

RECT CurrentRect(const SelState& s) {
  RECT r;
  r.left = std::min(s.a.x, s.b.x);
  r.top = std::min(s.a.y, s.b.y);
  r.right = std::max(s.a.x, s.b.x);
  r.bottom = std::max(s.a.y, s.b.y);
  return r;
}

HBITMAP CreateDib(HDC ref, int w, int h, uint32_t** bits) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  return CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(bits), nullptr, 0);
}

void DrawHint(HDC hdc, const SelState& s) {
  // Texto de ayuda centrado arriba, en el monitor principal.
  RECT mon = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  OffsetRect(&mon, -s.vx, -s.vy);

  const wchar_t* text = L"Drag to select the area to record   |   F: full screen   |   Esc: cancel";
  HFONT font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(hdc, font);
  SIZE sz{};
  GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);

  const int padX = 22, padY = 12;
  RECT box;
  box.left = mon.left + (mon.right - mon.left - sz.cx) / 2 - padX;
  box.top = mon.top + 28;
  box.right = box.left + sz.cx + padX * 2;
  box.bottom = box.top + sz.cy + padY * 2;

  HBRUSH br = CreateSolidBrush(RGB(13, 11, 43));
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(78, 234, 214));
  HGDIOBJ oldBr = SelectObject(hdc, br);
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  RoundRect(hdc, box.left, box.top, box.right, box.bottom, 24, 24);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBr);
  DeleteObject(pen);
  DeleteObject(br);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(120, 245, 255));
  TextOutW(hdc, box.left + padX, box.top + padY, text, (int)wcslen(text));
  SelectObject(hdc, oldFont);
  DeleteObject(font);
}

void Paint(HWND hwnd, SelState& s) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);

  // Fondo oscurecido.
  BitBlt(hdc, 0, 0, s.vw, s.vh, s.dcDim, 0, 0, SRCCOPY);

  if (s.hasRect) {
    const RECT r = CurrentRect(s);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w > 0 && h > 0) {
      // Zona elegida con el brillo original.
      BitBlt(hdc, r.left, r.top, w, h, s.dcOrig, r.left, r.top, SRCCOPY);

      HPEN pen = CreatePen(PS_SOLID, 2, RGB(78, 234, 214));
      HGDIOBJ oldPen = SelectObject(hdc, pen);
      HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, r.left, r.top, r.right, r.bottom);
      SelectObject(hdc, oldBr);
      SelectObject(hdc, oldPen);
      DeleteObject(pen);

      // Etiqueta con el tamaño.
      wchar_t label[64];
      swprintf(label, 64, L"%d × %d", w & ~1, h & ~1);
      HFONT font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HGDIOBJ oldFont = SelectObject(hdc, font);
      SIZE sz{};
      GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &sz);
      int lx = r.left;
      int ly = r.bottom + 8;
      if (ly + sz.cy + 12 > s.vh) ly = r.top - sz.cy - 14;
      if (ly < 0) ly = r.top + 8;
      RECT lb = {lx, ly, lx + sz.cx + 16, ly + sz.cy + 8};
      HBRUSH br = CreateSolidBrush(RGB(13, 11, 43));
      FillRect(hdc, &lb, br);
      DeleteObject(br);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(120, 245, 255));
      TextOutW(hdc, lx + 8, ly + 4, label, (int)wcslen(label));
      SelectObject(hdc, oldFont);
      DeleteObject(font);
    }
  }

  DrawHint(hdc, s);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK SelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  SelState* s = reinterpret_cast<SelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (s) Paint(hwnd, *s);
      return 0;
    case WM_SETCURSOR:
      SetCursor(g_selCursor ? g_selCursor : LoadCursorW(nullptr, IDC_CROSS));
      return TRUE;
    case WM_LBUTTONDOWN:
      if (s) {
        s->dragging = true;
        s->hasRect = true;
        s->a = s->b = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (s && s->dragging) {
        s->b = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (s && s->dragging) {
        s->dragging = false;
        ReleaseCapture();
        s->b = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const RECT r = CurrentRect(*s);
        if (r.right - r.left >= 32 && r.bottom - r.top >= 32) {
          s->result = r;
          s->ok = true;
          s->done = true;
        } else {
          s->hasRect = false;  // clic sin arrastrar: vuelve a intentar
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    case WM_RBUTTONDOWN:
      if (s) s->done = true;
      return 0;
    case WM_KEYDOWN:
      if (!s) return 0;
      if (wp == VK_ESCAPE) {
        s->done = true;
      } else if (wp == 'F') {
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          s->result = mi.rcMonitor;
          OffsetRect(&s->result, -s->vx, -s->vy);
          s->ok = true;
          s->done = true;
        }
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool SelectRegion(HINSTANCE hInst, RECT& out, int cursorScalePct, int cursorRgb) {
  SelState st;
  g_selCursor = MakeStyledCursor(LoadCursorW(nullptr, IDC_CROSS), cursorScalePct, cursorRgb);
  struct CursorGuard {
    ~CursorGuard() {
      if (g_selCursor) DestroyCursor(g_selCursor);
      g_selCursor = nullptr;
    }
  } cursorGuard;
  st.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  st.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  st.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  st.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  // Captura de todo el escritorio virtual + copia oscurecida.
  HDC screen = GetDC(nullptr);
  uint32_t* origBits = nullptr;
  uint32_t* dimBits = nullptr;

  st.dcOrig = CreateCompatibleDC(screen);
  st.dcDim = CreateCompatibleDC(screen);
  st.bmpOrig = CreateDib(screen, st.vw, st.vh, &origBits);
  st.bmpDim = CreateDib(screen, st.vw, st.vh, &dimBits);

  bool ready = st.bmpOrig && st.bmpDim && origBits && dimBits;
  if (ready) {
    st.oldOrig = SelectObject(st.dcOrig, st.bmpOrig);
    st.oldDim = SelectObject(st.dcDim, st.bmpDim);
    BitBlt(st.dcOrig, 0, 0, st.vw, st.vh, screen, st.vx, st.vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    const size_t n = (size_t)st.vw * st.vh;
    for (size_t i = 0; i < n; i++) dimBits[i] = (origBits[i] & 0xFEFEFEFEu) >> 1;  // 50% de brillo
  }
  ReleaseDC(nullptr, screen);

  auto cleanup = [&] {
    if (st.dcOrig) {
      if (st.oldOrig) SelectObject(st.dcOrig, st.oldOrig);
      DeleteDC(st.dcOrig);
    }
    if (st.dcDim) {
      if (st.oldDim) SelectObject(st.dcDim, st.oldDim);
      DeleteDC(st.dcDim);
    }
    if (st.bmpOrig) DeleteObject(st.bmpOrig);
    if (st.bmpDim) DeleteObject(st.bmpDim);
  };

  if (!ready) {
    cleanup();
    return false;
  }

  static bool registered = false;
  const wchar_t* kClass = L"MiauRegionSelector";
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SelProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
  }

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP, st.vx, st.vy, st.vw, st.vh,
                              nullptr, nullptr, hInst, &st);
  if (!hwnd) {
    cleanup();
    return false;
  }

  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);

  MSG msg;
  while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  DestroyWindow(hwnd);
  cleanup();

  if (!st.ok) return false;

  RECT r = st.result;
  OffsetRect(&r, st.vx, st.vy);
  // Ancho y alto pares.
  r.right = r.left + ((r.right - r.left) & ~1);
  r.bottom = r.top + ((r.bottom - r.top) & ~1);
  out = r;
  return true;
}
