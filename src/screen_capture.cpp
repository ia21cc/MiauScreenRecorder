#include "screen_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

static std::string HrToString(const char* what, HRESULT hr) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08X)", what, (unsigned)hr);
  return buf;
}

void ScreenCapture::Shutdown() {
  cur_ = CursorImg{};
  dupl_.Reset();
  staging_.Reset();
  ctx_.Reset();
  dev_.Reset();
  output_.Reset();
  adapter_.Reset();
  if (memDC_) {
    if (oldBmp_) SelectObject(memDC_, oldBmp_);
    DeleteDC(memDC_);
    memDC_ = nullptr;
  }
  if (dib_) {
    DeleteObject(dib_);
    dib_ = nullptr;
  }
  oldBmp_ = nullptr;
  bits_ = nullptr;
}

bool ScreenCapture::Init(RECT region, bool drawCursor, std::string& err) {
  Shutdown();
  cursor_ = drawCursor;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    err = HrToString("No se pudo crear la fábrica DXGI", hr);
    return false;
  }

  // Busca el monitor que más se solapa con la región.
  long long bestArea = -1;
  RECT desk{};
  for (UINT ai = 0;; ai++) {
    ComPtr<IDXGIAdapter1> ad;
    if (factory->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT oi = 0;; oi++) {
      ComPtr<IDXGIOutput> out;
      if (ad->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC d{};
      out->GetDesc(&d);
      RECT inter{};
      if (IntersectRect(&inter, &region, &d.DesktopCoordinates)) {
        const long long area = (long long)(inter.right - inter.left) * (inter.bottom - inter.top);
        if (area > bestArea) {
          ComPtr<IDXGIOutput1> out1;
          if (SUCCEEDED(out.As(&out1))) {
            bestArea = area;
            adapter_ = ad;
            output_ = out1;
            desk = d.DesktopCoordinates;
            region_ = inter;
          }
        }
      }
    }
  }

  if (bestArea <= 0 || !adapter_ || !output_) {
    err = "La región seleccionada no está sobre ningún monitor.";
    return false;
  }

  // Dimensiones pares (los codificadores YUV 4:2:0 las necesitan).
  int w = (region_.right - region_.left) & ~1;
  int h = (region_.bottom - region_.top) & ~1;
  if (w < 16 || h < 16) {
    err = "La región es demasiado pequeña.";
    return false;
  }
  region_.right = region_.left + w;
  region_.bottom = region_.top + h;
  w_ = w;
  h_ = h;
  local_ = {region_.left - desk.left, region_.top - desk.top, region_.right - desk.left,
            region_.bottom - desk.top};

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                         levels, (UINT)std::size(levels), D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
  if (FAILED(hr)) {
    err = HrToString("No se pudo crear el dispositivo D3D11", hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = (UINT)w_;
  td.Height = (UINT)h_;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = dev_->CreateTexture2D(&td, nullptr, &staging_);
  if (FAILED(hr)) {
    err = HrToString("No se pudo crear la textura intermedia", hr);
    return false;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w_;
  bmi.bmiHeader.biHeight = -h_;  // de arriba hacia abajo
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  dib_ = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits_), nullptr, 0);
  if (!dib_ || !bits_) {
    err = "No se pudo crear el búfer de imagen.";
    return false;
  }
  std::memset(bits_, 0, (size_t)w_ * h_ * 4);
  memDC_ = CreateCompatibleDC(nullptr);
  oldBmp_ = SelectObject(memDC_, dib_);

  if (!CreateDuplication(err)) return false;

  AcquireInto(300);  // primer fotograma para no empezar en negro
  return true;
}

bool ScreenCapture::CreateDuplication(std::string& err) {
  dupl_.Reset();
  HRESULT hr = output_->DuplicateOutput(dev_.Get(), &dupl_);
  if (FAILED(hr)) {
    if (hr == E_ACCESSDENIED)
      err = HrToString("Acceso denegado a la duplicación del escritorio (¿pantalla segura o UAC?)", hr);
    else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
      err = HrToString("Demasiadas aplicaciones usan la duplicación del escritorio", hr);
    else
      err = HrToString("No se pudo iniciar la captura de pantalla", hr);
    return false;
  }
  return true;
}

bool ScreenCapture::AcquireInto(UINT timeoutMs) {
  if (!dupl_) return false;

  DXGI_OUTDUPL_FRAME_INFO info{};
  ComPtr<IDXGIResource> res;
  HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);

  if (hr == DXGI_ERROR_ACCESS_LOST) {
    std::string ignored;
    CreateDuplication(ignored);  // cambio de resolución, bloqueo de pantalla, etc.
    return false;
  }
  if (FAILED(hr)) return false;  // timeout u otro: se reutiliza el fotograma anterior

  bool updated = false;
  if (info.LastPresentTime.QuadPart != 0) {  // si es 0 solo se movió el cursor
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
      D3D11_BOX box{};
      box.left = (UINT)local_.left;
      box.top = (UINT)local_.top;
      box.right = (UINT)local_.right;
      box.bottom = (UINT)local_.bottom;
      box.front = 0;
      box.back = 1;
      ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
      updated = true;
    }
  }
  dupl_->ReleaseFrame();
  return updated;
}

// Dibuja el cursor con otro tamaño/color directamente en el fotograma (el cursor real de Windows no se toca).
// Se pinta el cursor sobre fondo negro y sobre fondo blanco para recuperar su transparencia exacta.
void ScreenCapture::DrawStyledCursor(HCURSOR hc, const ICONINFO& ii, int hotX, int hotY) {
  if (cur_.h != hc || cur_.bgra.empty()) {
    cur_ = CursorImg{};
    BITMAP bm{};
    HBITMAP ref = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (!ref || !GetObject(ref, sizeof(bm), &bm)) return;
    const int bw = bm.bmWidth, bh = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    const int w = std::max(1, bw * cursorScale_ / 100), h = std::max(1, bh * cursorScale_ / 100);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint8_t* pb = nullptr;
    uint8_t* pw = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pb), nullptr, 0);
    HBITMAP bwb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pw), nullptr, 0);
    if (!bb || !bwb || !pb || !pw) {
      if (bb) DeleteObject(bb);
      if (bwb) DeleteObject(bwb);
      DeleteDC(dc);
      return;
    }
    const size_t n = (size_t)w * h;
    HGDIOBJ old = SelectObject(dc, bb);
    std::memset(pb, 0, n * 4);
    DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    SelectObject(dc, bwb);
    std::memset(pw, 0xFF, n * 4);
    DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    SelectObject(dc, old);

    cur_.h = hc;
    cur_.w = w;
    cur_.hh = h;
    cur_.hotX = (int)ii.xHotspot * cursorScale_ / 100;
    cur_.hotY = (int)ii.yHotspot * cursorScale_ / 100;
    cur_.bgra.assign(n * 4, 0);
    const int tr = cursorRgb_ >= 0 ? (cursorRgb_ >> 16) & 255 : 0, tg = cursorRgb_ >= 0 ? (cursorRgb_ >> 8) & 255 : 0,
              tb = cursorRgb_ >= 0 ? cursorRgb_ & 255 : 0;
    for (size_t i = 0; i < n; i++) {
      const uint8_t* b = pb + i * 4;
      const uint8_t* wv = pw + i * 4;
      const int diff = ((wv[0] - b[0]) + (wv[1] - b[1]) + (wv[2] - b[2])) / 3;
      const int a = std::clamp(255 - diff, 0, 255);
      if (a == 0) continue;
      int r = std::min(255, b[2] * 255 / a), g = std::min(255, b[1] * 255 / a), bl = std::min(255, b[0] * 255 / a);
      if (cursorRgb_ >= 0) {  // teñir: el relleno claro toma el color y el borde oscuro se conserva
        const int lum = (r * 77 + g * 151 + bl * 28) >> 8;
        r = tr * lum / 255;
        g = tg * lum / 255;
        bl = tb * lum / 255;
      }
      uint8_t* o = &cur_.bgra[i * 4];
      o[0] = (uint8_t)bl;
      o[1] = (uint8_t)g;
      o[2] = (uint8_t)r;
      o[3] = (uint8_t)a;
    }
    DeleteObject(bb);
    DeleteObject(bwb);
    DeleteDC(dc);
  }

  const int x0 = hotX - cur_.hotX, y0 = hotY - cur_.hotY;
  for (int y = 0; y < cur_.hh; y++) {
    const int dy = y0 + y;
    if (dy < 0 || dy >= h_) continue;
    for (int x = 0; x < cur_.w; x++) {
      const int dx = x0 + x;
      if (dx < 0 || dx >= w_) continue;
      const uint8_t* s = &cur_.bgra[((size_t)y * cur_.w + x) * 4];
      const int a = s[3];
      if (!a) continue;
      uint8_t* d = bits_ + ((size_t)dy * w_ + dx) * 4;
      d[0] = (uint8_t)((s[0] * a + d[0] * (255 - a)) / 255);
      d[1] = (uint8_t)((s[1] * a + d[1] * (255 - a)) / 255);
      d[2] = (uint8_t)((s[2] * a + d[2] * (255 - a)) / 255);
    }
  }
}

const uint8_t* ScreenCapture::Grab() {
  AcquireInto(0);

  // El staging conserva el último fotograma: se copia siempre al DIB.
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    const size_t rowBytes = (size_t)w_ * 4;
    for (int y = 0; y < h_; y++)
      std::memcpy(bits_ + (size_t)y * rowBytes, static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch,
                  rowBytes);
    ctx_->Unmap(staging_.Get(), 0);
  }

  if (cursor_) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
      ICONINFO ii{};
      if (GetIconInfo(ci.hCursor, &ii)) {
        const int x = ci.ptScreenPos.x - (int)ii.xHotspot - region_.left;
        const int y = ci.ptScreenPos.y - (int)ii.yHotspot - region_.top;
        if (cursorScale_ > 100 || cursorRgb_ >= 0) {
          GdiFlush();
          DrawStyledCursor(ci.hCursor, ii, ci.ptScreenPos.x - region_.left, ci.ptScreenPos.y - region_.top);
        } else {
          DrawIconEx(memDC_, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        }
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
      }
    }
    GdiFlush();
  }
  return bits_;
}
