#pragma once
// Crea un cursor de Windows (HCURSOR) con otro tamaño y color a partir de uno existente.
// Se usa en el selector de área para que el cursor elegido en Options se vea también al elegir el área.
// El cursor real del sistema no se modifica. Devuelve nullptr si no se pudo (se usa entonces el original).
#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline HCURSOR MakeStyledCursor(HCURSOR src, int scalePct, int rgb) {
  if (!src || (scalePct <= 100 && rgb < 0)) return nullptr;
  ICONINFO ii{};
  if (!GetIconInfo(src, &ii)) return nullptr;
  HCURSOR result = nullptr;
  BITMAP bm{};
  HBITMAP ref = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
  if (ref && GetObjectW(ref, sizeof(bm), &bm)) {
    const int bw = bm.bmWidth, bh = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    const int w = std::max(1, bw * std::max(100, scalePct) / 100), h = std::max(1, bh * std::max(100, scalePct) / 100);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    uint8_t *pb = nullptr, *pw = nullptr, *po = nullptr;
    HBITMAP bBlack = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pb), nullptr, 0);
    HBITMAP bWhite = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pw), nullptr, 0);
    HBITMAP bOut = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&po), nullptr, 0);
    if (bBlack && bWhite && bOut && pb && pw && po) {
      const size_t n = (size_t)w * h;
      HGDIOBJ old = SelectObject(dc, bBlack);
      std::memset(pb, 0, n * 4);
      DrawIconEx(dc, 0, 0, src, w, h, 0, nullptr, DI_NORMAL);
      GdiFlush();
      SelectObject(dc, bWhite);
      std::memset(pw, 0xFF, n * 4);
      DrawIconEx(dc, 0, 0, src, w, h, 0, nullptr, DI_NORMAL);
      GdiFlush();
      SelectObject(dc, old);
      const int tr = rgb >= 0 ? (rgb >> 16) & 255 : 0, tg = rgb >= 0 ? (rgb >> 8) & 255 : 0, tb = rgb >= 0 ? rgb & 255 : 0;
      for (size_t i = 0; i < n; i++) {  // transparencia exacta a partir del cursor sobre negro y sobre blanco
        const uint8_t* b = pb + i * 4;
        const uint8_t* wv = pw + i * 4;
        const int a = std::clamp(255 - ((wv[0] - b[0]) + (wv[1] - b[1]) + (wv[2] - b[2])) / 3, 0, 255);
        uint8_t* o = po + i * 4;
        if (!a) {
          o[0] = o[1] = o[2] = o[3] = 0;
          continue;
        }
        int r = std::min(255, b[2] * 255 / a), g = std::min(255, b[1] * 255 / a), bl = std::min(255, b[0] * 255 / a);
        if (rgb >= 0) {
          const int lum = (r * 77 + g * 151 + bl * 28) >> 8;
          r = tr * lum / 255;
          g = tg * lum / 255;
          bl = tb * lum / 255;
        }
        o[0] = (uint8_t)bl;
        o[1] = (uint8_t)g;
        o[2] = (uint8_t)r;
        o[3] = (uint8_t)a;
      }
      HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);  // máscara vacía: la transparencia va en el canal alfa
      ICONINFO ni{};
      ni.fIcon = FALSE;
      ni.xHotspot = (DWORD)((int)ii.xHotspot * std::max(100, scalePct) / 100);
      ni.yHotspot = (DWORD)((int)ii.yHotspot * std::max(100, scalePct) / 100);
      ni.hbmMask = mask;
      ni.hbmColor = bOut;
      result = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ni));
      if (mask) DeleteObject(mask);
    }
    if (bBlack) DeleteObject(bBlack);
    if (bWhite) DeleteObject(bWhite);
    if (bOut) DeleteObject(bOut);
    DeleteDC(dc);
  }
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  return result;
}

// ── Cursor de Windows con otro tamaño/color SOLO mientras se graba ──
// Se reemplazan los cursores del sistema (flecha, texto, mano, redimensionar...) por versiones con el tamaño y color
// elegidos y, al terminar, se restauran desde el registro de Windows. Así el usuario ve lo mismo que sale en el video.
// Un archivo marcador permite restaurarlos al abrir la app si esta se cerró sin poder hacerlo (p. ej. un fallo).
inline bool ApplySystemCursorStyle(int scalePct, int rgb, const std::wstring& markerPath) {
  static const WORD ids[] = {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650, 32651};
  if (scalePct <= 100 && rgb < 0) return false;
  HANDLE m = CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
  bool any = false;
  for (WORD id : ids) {
    HCURSOR src = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
    if (!src) continue;
    HCURSOR styled = MakeStyledCursor(src, scalePct, rgb);
    if (!styled) continue;
    if (SetSystemCursor(styled, id)) any = true;  // el sistema se queda con el cursor
    else DestroyCursor(styled);
  }
  return any;
}

inline void RestoreSystemCursors(const std::wstring& markerPath) {
  if (GetFileAttributesW(markerPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // no se cambió nada
  SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
  DeleteFileW(markerPath.c_str());
}
