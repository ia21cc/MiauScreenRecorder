#include "camera.h"

#include "log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstring>
#include <tuple>

using Microsoft::WRL::ComPtr;

std::vector<CameraInfo> ListCameras() {
  std::vector<CameraInfo> out;

  ComPtr<IMFAttributes> attr;
  if (FAILED(MFCreateAttributes(&attr, 1))) return out;
  attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

  IMFActivate** devs = nullptr;
  UINT32 n = 0;
  if (FAILED(MFEnumDeviceSources(attr.Get(), &devs, &n))) return out;

  for (UINT32 i = 0; i < n; i++) {
    WCHAR* name = nullptr;
    WCHAR* link = nullptr;
    UINT32 len = 0;
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
    if (link) {
      CameraInfo ci;
      ci.symlink = link;
      ci.name = name ? WideToUtf8(name) : "Camera";
      out.push_back(std::move(ci));
    }
    CoTaskMemFree(name);
    CoTaskMemFree(link);
    devs[i]->Release();
  }
  CoTaskMemFree(devs);
  return out;
}


bool CaptureStill(const std::wstring& link, std::vector<uint8_t>& out, int& ow, int& oh, std::string& err) {
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;

  // Intentos: la mayor resolución (con y sin procesamiento avanzado) y, si la cámara no entrega imagen
  // a esa resolución, las siguientes más grandes.
  const int kTries[][2] = {{0, 1}, {0, 0}, {1, 0}, {2, 0}, {3, 0}};  // {posición en la lista, avanzado}
  for (const auto& tr : kTries) {
    if (ok) break;
    const int rank = tr[0];
    const bool advanced = tr[1] != 0;
    ok = [&]() -> bool {
      ComPtr<IMFAttributes> attr;
      if (FAILED(MFCreateAttributes(&attr, 2))) return false;
      attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
      attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
      ComPtr<IMFMediaSource> source;
      if (FAILED(MFCreateDeviceSource(attr.Get(), &source))) return false;

      std::atomic<bool> watchDone{false}, got{false};
      std::thread watch([&] {
        for (int i = 0; i < 90 && !watchDone && !got; i++) Sleep(100);
        if (!watchDone && !got) source->Shutdown();  // destraba ReadSample si la cámara no entrega cuadros
      });
      struct Guard {
        std::atomic<bool>& d;
        std::thread& t;
        ~Guard() {
          d = true;
          if (t.joinable()) t.join();
        }
      } guard{watchDone, watch};

      ComPtr<IMFAttributes> ra;
      MFCreateAttributes(&ra, 2);
      ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
      if (advanced) ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
      ComPtr<IMFSourceReader> reader;
      if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader))) {
        source->Shutdown();
        return false;
      }

      const DWORD vs = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
      struct Cand {
        DWORD idx;
        UINT32 w, h;
        int rank;
      };
      std::vector<Cand> cands;
      for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(vs, i, &t))) break;
        UINT32 tw = 0, th = 0;
        GUID sub{};
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &tw, &th);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (!tw || !th) continue;
        const int rk = sub == MFVideoFormat_MJPG ? 3 : (sub == MFVideoFormat_NV12 ? 2 : 1);  // MJPG: más fps a alta resolución
        cands.push_back({i, tw, th, rk});
      }
      std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return std::make_tuple((long long)a.w * a.h, a.rank) > std::make_tuple((long long)b.w * b.h, b.rank);
      });
      if (cands.empty()) {
        source->Shutdown();
        return false;
      }
      const Cand& c = cands[(size_t)std::min<int>(rank, (int)cands.size() - 1)];

      ComPtr<IMFMediaType> nat;
      if (FAILED(reader->GetNativeMediaType(vs, c.idx, &nat)) || FAILED(reader->SetCurrentMediaType(vs, nullptr, nat.Get()))) {
        source->Shutdown();
        return false;
      }
      ComPtr<IMFMediaType> want;
      MFCreateMediaType(&want);
      want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
      MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, c.w, c.h);
      ComPtr<IMFMediaType> cur;
      if (FAILED(reader->SetCurrentMediaType(vs, nullptr, want.Get())) || FAILED(reader->GetCurrentMediaType(vs, &cur))) {
        source->Shutdown();
        return false;
      }
      UINT32 w = 0, h = 0;
      MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
      if (!w || !h) {
        source->Shutdown();
        return false;
      }
      LONG stride = (LONG)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0);
      if (stride == 0) stride = -(LONG)(w * 4);
      const bool bottomUp = stride < 0;
      const size_t rowBytes = (size_t)std::abs((long)stride);

      // Se descartan los primeros cuadros (la cámara ajusta exposición y enfoque) y se conserva el último.
      std::vector<uint8_t> frame((size_t)w * h * 4);
      bool have = false;
      ULONGLONG firstAt = 0;
      for (int n = 0; n < 200; n++) {
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(vs, 0, &si, &flags, &ts, &sample)) || (flags & MF_SOURCE_READERF_ERROR) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
          break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&p, &maxLen, &curLen))) continue;
        if (curLen >= rowBytes * h) {
          for (UINT32 y = 0; y < h; y++) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(p + rowBytes * (bottomUp ? (h - 1 - y) : y));
            uint32_t* d = reinterpret_cast<uint32_t*>(frame.data() + (size_t)y * w * 4);
            for (UINT32 x = 0; x < w; x++) d[x] = s[x] | 0xFF000000u;
          }
          have = true;
          got = true;
          if (!firstAt) firstAt = GetTickCount64();
        }
        buf->Unlock();
        if (have && GetTickCount64() - firstAt > 1500) break;
      }
      watchDone = true;
      if (!have) {
        source->Shutdown();
        return false;
      }
      out = std::move(frame);
      ow = (int)w;
      oh = (int)h;
      LogLine("Foto: %ux%u (posicion %d, avanzado %d)", w, h, rank, (int)advanced);
      source->Shutdown();
      return true;
    }();
    if (!ok) Sleep(300);
  }

  if (!ok) err = "La cámara no entregó imagen. Comprueba que ninguna otra aplicación la esté usando.";
  if (SUCCEEDED(hrCo)) CoUninitialize();
  return ok;
}

void Camera::Start(const std::wstring& symlink) {
  Stop();
  stop_ = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    frame_.clear();
    w_ = h_ = 0;
    serial_ = 0;
  }
  th_ = std::thread(&Camera::Run, this, symlink);
}

void Camera::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  running_ = false;
}

bool Camera::GetFrame(std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& serial) {
  std::lock_guard<std::mutex> lk(mu_);
  if (frame_.empty() || serial == serial_) return false;
  bgra = frame_;
  w = w_;
  h = h_;
  serial = serial_;
  return true;
}

void Camera::Run(std::wstring link) {
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // mode 0: mejor resolución (hasta Full HD); 1: máx. 1280x720; 2: tipo por defecto de Media Foundation.
  auto attempt = [&](int mode) -> bool {
    bool gotFrame = false;
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 2))) return false;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());

    ComPtr<IMFMediaSource> source;
    {
      const HRESULT hr = MFCreateDeviceSource(attr.Get(), &source);
      if (FAILED(hr)) {
        lastHr_ = (long)hr;
        LogLine("Camera: MFCreateDeviceSource fallo 0x%08lx", (unsigned long)hr);
        return false;
      }
    }

    // Vigilante que cubre TODO el intento (configuración y espera del primer cuadro): si en 8 s no llega
    // ningún cuadro, cierra la fuente para destrabar cualquier llamada bloqueada y probar otro modo.
    std::atomic<bool> watchDone{false}, got{false};
    std::thread watch([&] {
      for (int i = 0; i < 80 && !stop_ && !watchDone && !got; i++) Sleep(100);
      if (!stop_ && !watchDone && !got) {
        LogLine("Camera: sin cuadros en 8 s (modo %d, paso %s), reintentando", mode, stage_.load());
        source->Shutdown();
      }
    });
    struct WatchGuard {
      std::atomic<bool>& done;
      std::thread& th;
      ~WatchGuard() {
        done = true;
        if (th.joinable()) th.join();
      }
    } watchGuard{watchDone, watch};
    stage_ = "configurando";

    ComPtr<IMFAttributes> readerAttr;
    MFCreateAttributes(&readerAttr, 2);
    readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (mode != 2) readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    stage_ = "lector";
    ComPtr<IMFSourceReader> reader;
    if (const HRESULT hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttr.Get(), &reader); FAILED(hr)) {
      lastHr_ = (long)hr;
      LogLine("Camera: MFCreateSourceReader fallo 0x%08lx (modo %d)", (unsigned long)hr, mode);
      source->Shutdown();
      return false;
    }

    // Elegimos la mejor resolución nativa (hasta Full HD) que mantenga ~30 fps; si no, la mayor posible.
    // Sin esto Media Foundation suele quedarse con 640x480.
    stage_ = "tipos";
    const DWORD vs = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    struct Cand {
      DWORD idx;
      UINT32 w, h;
      double fps;
      int rank;
      const char* name;
    };
    std::vector<Cand> cands;
    for (DWORD i = 0; mode < 2; i++) {
      ComPtr<IMFMediaType> t;
      if (FAILED(reader->GetNativeMediaType(vs, i, &t))) break;
      UINT32 tw = 0, th = 0, n = 0, d = 1;
      GUID sub{};
      MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &tw, &th);
      MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &n, &d);
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      if (!tw || !th) continue;
      if (mode == 1 && (tw > 1280 || th > 720)) continue;
      int rank = 0;
      const char* nm = "other";
      if (sub == MFVideoFormat_NV12) { rank = 3; nm = "NV12"; }
      else if (sub == MFVideoFormat_YUY2) { rank = 2; nm = "YUY2"; }
      else if (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_RGB24) { rank = 2; nm = "RGB"; }
      else if (sub == MFVideoFormat_MJPG) { rank = 1; nm = "MJPG"; }
      cands.push_back({i, tw, th, d ? (double)n / d : 0.0, rank, nm});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      auto key = [](const Cand& c) {
        const bool inLimit = c.w <= 1920 && c.h <= 1088;
        const bool fpsOk = c.fps >= 27.5;
        return std::make_tuple(inLimit, fpsOk, (long long)c.w * c.h, c.rank, c.fps);
      };
      return key(a) > key(b);
    });

    stage_ = "formato";
    ComPtr<IMFMediaType> cur;
    bool configured = false;
    int tried = 0;
    for (const Cand& c : cands) {
      if (tried++ >= 8) break;
      ComPtr<IMFMediaType> nat;
      if (FAILED(reader->GetNativeMediaType(vs, c.idx, &nat))) continue;
      if (const HRESULT hr = reader->SetCurrentMediaType(vs, nullptr, nat.Get()); FAILED(hr)) {
        lastHr_ = (long)hr;
        LogLine("Camera: tipo nativo %ux%u %s rechazado 0x%08lx", c.w, c.h, c.name, (unsigned long)hr);
        continue;
      }
      ComPtr<IMFMediaType> want;
      MFCreateMediaType(&want);
      want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);  // el procesamiento de video convierte a RGB32
      MFSetAttributeSize(want.Get(), MF_MT_FRAME_SIZE, c.w, c.h);
      {
        const HRESULT hr = reader->SetCurrentMediaType(vs, nullptr, want.Get());
        if (FAILED(hr)) {
          lastHr_ = (long)hr;
          LogLine("Camera: %ux%u %s rechazado 0x%08lx", c.w, c.h, c.name, (unsigned long)hr);
          continue;
        }
      }
      cur.Reset();
      if (FAILED(reader->GetCurrentMediaType(vs, &cur))) continue;
      LogLine("Camera: %ux%u @ %.1f fps (%s)", c.w, c.h, c.fps, c.name);
      configured = true;
      break;
    }
    if (!configured) {  // respaldo: lo que decida Media Foundation
      ComPtr<IMFMediaType> want;
      MFCreateMediaType(&want);
      want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
      if (const HRESULT hr = reader->SetCurrentMediaType(vs, nullptr, want.Get()); FAILED(hr)) {
        lastHr_ = (long)hr;
        LogLine("Camera: tipo por defecto rechazado 0x%08lx", (unsigned long)hr);
        source->Shutdown();
        return false;
      }
      cur.Reset();
      if (FAILED(reader->GetCurrentMediaType(vs, &cur))) {
        source->Shutdown();
        return false;
      }
      LogLine("Camera: tipo por defecto (no se pudo elegir uno)");
    }

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (w == 0 || h == 0) {
      source->Shutdown();
      return false;
    }

    // Un stride negativo significa imagen de abajo hacia arriba.
    LONG stride = (LONG)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0);
    if (stride == 0) stride = -(LONG)(w * 4);
    const bool bottomUp = stride < 0;
    const size_t rowBytes = (size_t)std::abs((long)stride);

    running_ = true;
    stage_ = "esperando imagen";
    std::vector<uint8_t> tmp((size_t)w * h * 4);

    while (!stop_) {
      DWORD streamIndex = 0, flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample);
      if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
      if (!sample) continue;

      ComPtr<IMFMediaBuffer> buf;
      if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;

      BYTE* p = nullptr;
      DWORD maxLen = 0, curLen = 0;
      if (FAILED(buf->Lock(&p, &maxLen, &curLen))) continue;

      if (curLen >= rowBytes * h) {
        for (UINT32 y = 0; y < h; y++) {
          const BYTE* srcRow = p + rowBytes * (bottomUp ? (h - 1 - y) : y);
          uint32_t* dstRow = reinterpret_cast<uint32_t*>(tmp.data() + (size_t)y * w * 4);
          const uint32_t* s = reinterpret_cast<const uint32_t*>(srcRow);
          for (UINT32 x = 0; x < w; x++) dstRow[x] = s[x] | 0xFF000000u;  // alpha opaco
        }
        buf->Unlock();

        std::lock_guard<std::mutex> lk(mu_);
        frame_ = tmp;
        w_ = (int)w;
        h_ = (int)h;
        serial_++;
        got = true;
        gotFrame = true;
      } else {
        buf->Unlock();
      }
    }

    source->Shutdown();
    return gotFrame;
  };

  // Reintenta sin límite (la cámara puede estar ocupada o tardar en arrancar) alternando modos.
  static const int kModes[] = {0, 2, 1};
  for (int n = 0; !stop_; n++) {
    const int mode = kModes[n % 3];
    attempt_ = n + 1;
    mode_ = mode;
    stage_ = "abriendo";
    LogLine("Camera: intento %d (modo %d)", n + 1, mode);
    if (attempt(mode)) break;
    for (int i = 0; i < 10 && !stop_; i++) Sleep(100);
  }


  running_ = false;
  if (SUCCEEDED(hrCo)) CoUninitialize();
}
