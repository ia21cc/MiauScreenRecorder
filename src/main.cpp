// Miau Screen Recorder — ventana principal (Win32 + Direct3D 11 + Dear ImGui)
#include "common.h"

#include <windowsx.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "audio.h"
extern "C" {
#include <libavutil/avutil.h>
}
#include "camera.h"
#include "cursor_style.h"
#include "log.h"
#include "photo.h"
#include "logo_rgba.h"
#include "overlays.h"
#include "recorder.h"
#include "region_selector.h"
#include "settings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;

namespace {

// ─────────────────────────────── Colores ────────────────────────────────
const ImU32 kBg = IM_COL32(11, 15, 24, 255);
const ImU32 kBorder = IM_COL32(110, 225, 240, 235);
const ImU32 kPanelBorder = IM_COL32(60, 200, 215, 255);
const ImU32 kCyan = IM_COL32(94, 242, 236, 255);
const ImU32 kMagenta = IM_COL32(214, 100, 240, 255);
const ImU32 kViolet = IM_COL32(150, 95, 250, 255);
const ImU32 kText = IM_COL32(225, 236, 250, 255);
const ImU32 kTextDim = IM_COL32(140, 158, 192, 255);
const ImU32 kSegOn = IM_COL32(60, 227, 180, 255);
const ImU32 kSegWarn = IM_COL32(250, 220, 90, 255);
const ImU32 kSegHot = IM_COL32(255, 85, 100, 255);

constexpr float kWinW = 870.f;  // tamaño lógico (a 100 % de escala)
constexpr float kWinH = 531.f;
constexpr float kTitleH = 84.f;

struct Meter {
  float lvl = 0.f, hold = 0.f, holdT = 0.f;
};

struct Tex {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  int w = 0, h = 0;
};

// ───────────────────────────── Aplicación ───────────────────────────────
struct App {
  HINSTANCE hInst = nullptr;
  HWND hwnd = nullptr;
  float scale = 1.f;

  // Direct3D
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain> swap;
  ComPtr<ID3D11RenderTargetView> rtv;
  bool occluded = false;

  // Fuentes
  ImFont* fBody = nullptr;
  ImFont* fHead = nullptr;
  ImFont* fTitle = nullptr;
  float szBody = 17.f, szHead = 25.f, szTitle = 40.f;  // tamaños en píxeles

  // Ajustes y dispositivos
  Settings st;
  std::vector<AudioDevice> outs, ins;
  std::vector<CameraInfo> cams;
  std::vector<std::string> dLabels, mLabels, cLabels;
  std::vector<std::wstring> dIds, mIds, cIds;

  AudioCapture desktop, mic;
  Camera camera;
  CameraWindow camWin;
  StopWindow stopWin;
  CameraControls camCtl;
  Recorder rec;

  std::wstring appliedDesktop = L"\x1", appliedMic = L"\x1", appliedCam = L"\x1";
  Meter mDesktop, mMic;
  float micBar = 0.f;  // nivel suavizado de la barra bajo la cámara
  int regionW = 0;     // ancho del área que se está grabando
  bool camShown = false;

  Tex logo, camTex;
  std::vector<uint8_t> camBuf, camSmall;
  uint64_t camSerial = 0;
  float camAspect = 16.f / 9.f;

  // Estado del flujo
  enum class Mode { Idle, Recording, Saving } mode = Mode::Idle;
  bool wantStart = false, wantStop = false, finished = false, wantBrowse = false;
  bool wantRefresh = true, showOptions = false, showAbout = false, dirty = false;
  std::thread stopThread;
  std::thread photoThread, camStopThread;
  Recorder camRec;  // grabación de solo cámara + micrófono
  bool camRecording = false, camSaving = false, photoOverlay = false;
  std::atomic<bool> photoBusy{false};
  bool photoFast = false;
  bool cursorsStyled = false;
  std::wstring photoPath;
  std::string photoErr;
  bool photoOk = false;

  std::string status;
  bool statusError = false;
  std::wstring lastFile;
  char outDirBuf[520] = {};

  float S(float v) const { return v * scale; }
};

App g;

void CopyStr(char* dst, size_t cap, const std::string& src) {
  const size_t n = std::min(cap - 1, src.size());
  memcpy(dst, src.data(), n);
  dst[n] = 0;
}

// ───────────────────────────── Direct3D ─────────────────────────────────
bool CreateDevice() {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = g.hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                             &sd, &g.swap, &g.dev, &got, &g.ctx);
  if (FAILED(hr))
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                       &g.swap, &g.dev, &got, &g.ctx);
  return SUCCEEDED(hr);
}

void CreateTarget() {
  ComPtr<ID3D11Texture2D> back;
  if (SUCCEEDED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) g.dev->CreateRenderTargetView(back.Get(), nullptr, &g.rtv);
}

void ResizeTarget() {
  if (!g.swap) return;
  g.rtv.Reset();
  g.swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
  CreateTarget();
}

bool MakeTexture(Tex& t, const uint8_t* px, int w, int h, DXGI_FORMAT fmt) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (UINT)w;
  d.Height = (UINT)h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sr{px, (UINT)(w * 4), 0};
  t.tex.Reset();
  t.srv.Reset();
  if (FAILED(g.dev->CreateTexture2D(&d, px ? &sr : nullptr, &t.tex))) return false;
  if (FAILED(g.dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return false;
  t.w = w;
  t.h = h;
  return true;
}

ImTextureID TexId(const Tex& t) { return (ImTextureID)(intptr_t)t.srv.Get(); }

// ─────────────────────────── Dispositivos ───────────────────────────────
int IndexOf(const std::vector<std::wstring>& ids, const std::wstring& id, int fallback) {
  for (size_t i = 0; i < ids.size(); i++)
    if (ids[i] == id) return (int)i;
  return fallback;
}

bool CamOff() { return g.st.cameraLink.empty() || g.st.cameraLink == L"none"; }

// Primera cámara real: evita cámaras virtuales (OBS, etc.) si hay otra.
std::wstring PickDefaultCamera() {
  auto isVirtual = [](const std::string& n) {
    std::string l = n;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return l.find("virtual") != std::string::npos || l.find("obs") != std::string::npos ||
           l.find("snap camera") != std::string::npos || l.find("manycam") != std::string::npos;
  };
  for (auto& c : g.cams)
    if (!isVirtual(c.name)) return c.symlink;
  return g.cams.empty() ? std::wstring() : g.cams[0].symlink;
}

void RefreshDevices() {
  g.outs = ListAudioDevices(true);
  g.ins = ListAudioDevices(false);
  g.cams = ListCameras();

  g.dLabels = {"Off (don't record desktop audio)", "Default output device"};
  g.dIds = {L"", L"default"};
  for (auto& d : g.outs) {
    g.dLabels.push_back(d.name);
    g.dIds.push_back(d.id);
  }

  g.mLabels = {"Off (no microphone)", "Default input device"};
  g.mIds = {L"", L"default"};
  for (auto& d : g.ins) {
    g.mLabels.push_back(d.name);
    g.mIds.push_back(d.id);
  }

  g.cLabels = {"No camera"};
  g.cIds = {L"none"};
  for (auto& c : g.cams) {
    g.cLabels.push_back(c.name);
    g.cIds.push_back(c.symlink);
  }

  // Si el dispositivo guardado ya no existe, vuelve al predeterminado.
  if (IndexOf(g.dIds, g.st.desktopId, -1) < 0) g.st.desktopId = L"default";
  if (IndexOf(g.mIds, g.st.micId, -1) < 0) g.st.micId = L"default";

  // Cámara: "" = sin elegir todavía (se toma una disponible), "none" = el usuario eligió no usar cámara.
  if (g.mode == App::Mode::Idle && g.st.cameraLink != L"none") {
    if (IndexOf(g.cIds, g.st.cameraLink, -1) < 0) g.st.cameraLink = L"";  // la guardada ya no está
    if (g.st.cameraLink.empty() && !g.cams.empty()) {
      g.st.cameraLink = PickDefaultCamera();
      g.dirty = true;
    }
  }
  g.wantRefresh = false;
}

// Inicia o detiene las capturas cuando cambia la selección.
void SyncDevices() {
  if (g.st.desktopId != g.appliedDesktop) {
    g.appliedDesktop = g.st.desktopId;
    if (g.st.desktopId.empty()) g.desktop.Stop();
    else g.desktop.Start(g.st.desktopId, true);
  }
  if (g.st.micId != g.appliedMic) {
    g.appliedMic = g.st.micId;
    if (g.st.micId.empty()) g.mic.Stop();
    else g.mic.Start(g.st.micId, false);
  }
  if (g.st.cameraLink != g.appliedCam) {
    g.appliedCam = g.st.cameraLink;
    g.camTex = Tex{};
    g.camSerial = 0;
    if (CamOff()) g.camera.Stop();
    else g.camera.Start(g.st.cameraLink);
  }
}

std::wstring PickFolder(HWND owner) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return L"";
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  if (FAILED(dlg->Show(owner))) return L"";
  ComPtr<IShellItem> item;
  if (FAILED(dlg->GetResult(&item))) return L"";
  PWSTR p = nullptr;
  std::wstring out;
  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) out = p;
  CoTaskMemFree(p);
  return out;
}

// Colores del cursor en el video (solo afectan a la grabación; el cursor de Windows no cambia).
struct CursorColor {
  const char* name;
  int rgb;  // 0xRRGGBB; -1 = original
};
const CursorColor kCursorColors[] = {{"Original", -1},  {"White", 0xFFFFFF}, {"Yellow", 0xFFE600}, {"Red", 0xFF3B3B},
                                     {"Orange", 0xFF9A1F}, {"Green", 0x3CE36B}, {"Cyan", 0x2FE6F2},  {"Blue", 0x4C8DFF},
                                     {"Magenta", 0xFF4FD8}, {"Black", 0x101010}};
constexpr int kCursorColorCount = (int)(sizeof(kCursorColors) / sizeof(kCursorColors[0]));

// ──────────────────────── Flujo de grabación ────────────────────────────
void SetStatus(const std::string& s, bool error) {
  g.status = s;
  g.statusError = error;
}

void ShowMain() {
  ShowWindow(g.hwnd, SW_SHOW);
  if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
  SetForegroundWindow(g.hwnd);
}

std::wstring CursorMarkerPath() {
  const std::wstring lp = LogPath();
  const size_t k = lp.find_last_of(L'\\');
  return (k == std::wstring::npos ? std::wstring() : lp.substr(0, k + 1)) + L"cursors_changed.flag";
}

void BeginRecordingFlow() {
  g.st.Save();
  g.lastFile.clear();
  SetStatus("", false);

  ShowWindow(g.hwnd, SW_HIDE);  // que la ventana no estorbe ni salga en el video
  Sleep(150);

  RECT region{};
  if (!SelectRegion(g.hInst, region, g.st.cursor ? g.st.cursorScalePct : 100,
                    g.st.cursor ? kCursorColors[std::clamp(g.st.cursorColor, 0, kCursorColorCount - 1)].rgb : -1)) {
    ShowMain();
    SetStatus("Selection cancelled.", false);
    return;
  }

  RecorderSettings rs;
  rs.outputDir = g.st.outputDir;
  rs.mp4 = g.st.mp4;
  rs.fps = g.st.fps;
  rs.quality = g.st.quality;
  rs.cursor = g.st.cursor;
  rs.cursorScalePct = g.st.cursorScalePct;
  rs.cursorRgb = kCursorColors[std::clamp(g.st.cursorColor, 0, kCursorColorCount - 1)].rgb;
  // Mientras se graba, el cursor de Windows se cambia al tamaño/color elegidos para que se vea igual que en el video.
  // Se restaura al detener. Si funciona, el video ya recibe el cursor con ese aspecto (no se vuelve a estilizar).
  g.cursorsStyled = false;
  if (g.st.cursor && (rs.cursorScalePct > 100 || rs.cursorRgb >= 0) &&
      ApplySystemCursorStyle(rs.cursorScalePct, rs.cursorRgb, CursorMarkerPath())) {
    g.cursorsStyled = true;
    rs.cursorScalePct = 100;
    rs.cursorRgb = -1;
  }
  rs.separateTracks = g.st.separateTracks;

  std::string err;
  if (!g.rec.Start(region, rs, g.st.desktopId.empty() ? nullptr : &g.desktop, g.st.micId.empty() ? nullptr : &g.mic,
                   err)) {
    RestoreSystemCursors(CursorMarkerPath());
    ShowMain();
    SetStatus("Could not start recording: " + err, true);
    return;
  }

  const int rw = region.right - region.left;
  const int margin = (int)g.S(24);

  // Cámara flotante con su botón de 3 puntos.
  g.regionW = rw;
  g.camShown = false;
  if (!CamOff() && g.camera.Running()) {
    const int cw = std::clamp(rw * g.st.camSizePct / 100, (int)g.S(160), std::max((int)g.S(160), rw / 2));
    const int ch = (int)(cw / g.camAspect);
    // Aparece abajo a la derecha; después se arrastra a cualquier lugar.
    g.camWin.Show(region.right - cw - margin, region.bottom - ch - margin, cw, g.camAspect);
    g.camCtl.SetMic(g.st.micId.empty() ? nullptr : &g.mic);
    g.camCtl.Begin();
    g.camShown = true;
  }

  // Botón STOP: arriba al centro del área. No sale en el video.
  g.stopWin.Show(region.left + (rw - g.stopWin.Width()) / 2, region.top + margin, g.rec.StartTime());
  g.mode = App::Mode::Recording;
}

void EndRecordingFlow() {
  RestoreSystemCursors(CursorMarkerPath());
  // Recuerda el tamaño en el que dejaste la cámara para la próxima grabación.
  if (g.camShown && g.regionW > 0) {
    g.st.camSizePct = std::clamp(g.camWin.Width() * 100 / g.regionW, 8, 60);
    g.dirty = true;
  }
  g.camCtl.End();
  g.camWin.Hide();
  g.stopWin.Hide();
  g.mode = App::Mode::Saving;
  g.stopThread = std::thread([] {
    g.rec.Stop();  // puede tardar unos segundos (remux a MP4)
    PostMessageW(g.hwnd, WM_APP_FINISHED, 0, 0);
  });
}

// ── Foto de la cámara ──
// Se detiene la vista previa, se abre la cámara a su máxima resolución, se toma una foto y se guarda en la misma
// carpeta que las grabaciones (así "Show in folder" la selecciona). Después se reanuda la vista previa.
void StartPhoto() {
  if (g.photoBusy || CamOff() || g.mode != App::Mode::Idle) return;
  if (g.photoThread.joinable()) g.photoThread.join();
  g.photoBusy = true;
  g.photoOverlay = true;  // "Saving image..." sobre la vista previa hasta que vuelva a estar disponible
  g.photoOk = false;
  const std::wstring link = g.st.cameraLink;
  const std::wstring dir = g.st.outputDir;

  // Modo rápido (por defecto): se guarda al instante el último cuadro de la vista previa; la cámara no se interrumpe.
  g.photoFast = !g.st.photoMax;
  if (g.photoFast) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (g.camera.Snapshot(px, w, h)) {
      SetStatus("Saving image...", false);
      g.photoThread = std::thread([px = std::move(px), w, h, dir]() mutable {
        std::string err;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[96];
        swprintf(name, 96, L"Miau_Photo_%04d-%02d-%02d_%02d-%02d-%02d.jpg", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\" + name;
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ok = SaveJpeg(path, px.data(), w, h, 95, err);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        g.photoOk = ok;
        if (ok) {
          g.photoPath = path;
          g.photoErr = std::to_string(w) + "x" + std::to_string(h);
        } else {
          g.photoErr = err;
        }
        PostMessageW(g.hwnd, WM_APP_PHOTO, 0, 0);
      });
      return;
    }
    g.photoFast = false;  // aún no hay cuadro: se usa el modo lento
  }
  SetStatus("Taking photo...", false);
  g.photoThread = std::thread([link, dir] {
    g.camera.Stop();  // la cámara solo puede abrirla un lector a la vez
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    std::string err;
    bool ok = CaptureStill(link, px, w, h, err);
    if (ok) {
      SYSTEMTIME t;
      GetLocalTime(&t);
      wchar_t name[96];
      swprintf(name, 96, L"Miau_Photo_%04d-%02d-%02d_%02d-%02d-%02d.jpg", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
      CreateDirectoryW(dir.c_str(), nullptr);
      const std::wstring path = dir + L"\\" + name;
      const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      ok = SaveJpeg(path, px.data(), w, h, 95, err);
      if (SUCCEEDED(hrCo)) CoUninitialize();
      if (ok) {
        g.photoPath = path;
        g.photoErr = std::to_string(w) + "x" + std::to_string(h);
      }
    }
    g.photoOk = ok;
    if (!ok) g.photoErr = err;
    PostMessageW(g.hwnd, WM_APP_PHOTO, 0, 0);
  });
}

void FinishPhoto() {
  if (g.photoThread.joinable()) g.photoThread.join();
  g.photoBusy = false;
  if (g.photoOk) {
    g.lastFile = g.photoPath;
    SetStatus("Photo saved (" + g.photoErr + "): " + WideToUtf8(g.photoPath), false);
  } else {
    SetStatus("Could not take the photo: " + g.photoErr, true);
  }
  if (g.photoFast) {  // la cámara nunca se detuvo: el aviso se quita al instante
    g.photoOverlay = false;
    return;
  }
  // Reanuda la vista previa (se conserva el último cuadro hasta que llegue uno nuevo).
  g.camSerial = 0;
  if (!CamOff()) g.camera.Start(g.st.cameraLink);
}

// ── Video de la cámara (con el micrófono seleccionado) ──
void StartCameraVideo() {
  if (g.camRecording || g.camSaving || g.photoBusy || g.mode != App::Mode::Idle || CamOff() || !g.camera.Running()) return;
  RecorderSettings rs;
  rs.outputDir = g.st.outputDir;
  rs.mp4 = g.st.mp4;
  rs.quality = g.st.quality;
  std::string err;
  if (!g.camRec.StartCamera(&g.camera, rs, g.st.micId.empty() ? nullptr : &g.mic, err)) {
    SetStatus("Could not record the camera: " + err, true);
    return;
  }
  g.camRecording = true;
  SetStatus("", false);
}

void StopCameraVideo() {
  if (!g.camRecording) return;
  g.camRecording = false;
  g.camSaving = true;
  if (g.camStopThread.joinable()) g.camStopThread.join();
  g.camStopThread = std::thread([] {
    g.camRec.Stop();  // puede tardar unos segundos (convierte a MP4)
    PostMessageW(g.hwnd, WM_APP_CAMDONE, 0, 0);
  });
}

void FinishCameraVideo() {
  if (g.camStopThread.joinable()) g.camStopThread.join();
  g.camSaving = false;
  const RecorderResult r = g.camRec.Result();
  if (r.ok) {
    g.lastFile = r.path;
    SetStatus(r.message.empty() ? "Camera video saved: " + WideToUtf8(r.path) : r.message, !r.message.empty());
  } else {
    SetStatus(r.message.empty() ? "The camera video failed." : r.message, true);
  }
}

// Tras cada grabación la app se reinicia sola: Windows deja restos en la captura de pantalla / ventanas excluidas
// (recuadro negro desde la 2.ª grabación) y una instancia nueva empieza siempre limpia. Los ajustes ya están guardados,
// y el aviso "Saved: ..." y la posición de la ventana pasan a la instancia nueva.
std::wstring RestartFile() {
  const std::wstring lp = LogPath();
  const size_t k = lp.find_last_of(L'\\');
  return (k == std::wstring::npos ? std::wstring() : lp.substr(0, k + 1)) + L"restart.txt";
}

bool RestartSelf(const std::string& status, bool error, const std::wstring& file) {
  g.st.Save();
  FILE* f = nullptr;
  _wfopen_s(&f, RestartFile().c_str(), L"wb");
  if (!f) return false;
  const std::string fp = WideToUtf8(file);
  fprintf(f, "%d\n%s\n%s\n", error ? 1 : 0, fp.c_str(), status.c_str());
  fclose(f);

  RECT wr{};
  GetWindowRect(g.hwnd, &wr);
  wchar_t exe[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --restart " + std::to_wstring(GetCurrentProcessId()) + L" " +
                     std::to_wstring(wr.left) + L" " + std::to_wstring(wr.top);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void FinishFlow() {
  if (g.stopThread.joinable()) g.stopThread.join();
  const RecorderResult r = g.rec.Result();
  g.mode = App::Mode::Idle;
  if (r.ok) {
    g.lastFile = r.path;
    SetStatus(r.message.empty() ? "Saved: " + WideToUtf8(r.path) : r.message, !r.message.empty());
  } else {
    SetStatus(r.message.empty() ? "The recording failed." : r.message, true);
  }
  if (RestartSelf(g.status, g.statusError, g.lastFile)) {
    ShowWindow(g.hwnd, SW_HIDE);
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);  // la instancia nueva espera a que esta termine
    return;
  }
  ShowMain();  // si no se pudo reiniciar, se sigue en esta instancia
}

// ───────────────────────────── Widgets ──────────────────────────────────
ImU32 Lerp32(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int sh) { return (int)(((a >> sh) & 255) * (1.f - t) + ((b >> sh) & 255) * t); };
  return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), ch(IM_COL32_A_SHIFT));
}

// Borde redondeado con degradado horizontal (por franjas recortadas). Solo visual.
void GradRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 ca, ImU32 cb, float rounding, float th) {
  const int n = 48;
  const float w = p1.x - p0.x, pad = th * 2;
  for (int i = 0; i < n; i++) {
    const float x0 = p0.x - pad + (w + 2 * pad) * i / n, x1 = p0.x - pad + (w + 2 * pad) * (i + 1) / n + 1.f;
    dl->PushClipRect(ImVec2(x0, p0.y - pad), ImVec2(x1, p1.y + pad), true);
    dl->AddRect(p0, p1, Lerp32(ca, cb, (i + 0.5f) / n), rounding, 0, th);
    dl->PopClipRect();
  }
}

void DrawPanel(ImDrawList* dl, const char* title, ImVec2 p, ImVec2 s) {
  GradRect(dl, p, p + s, kPanelBorder, kMagenta, g.S(16), g.S(2));
  const float fs = g.szHead;
  const ImVec2 ts = g.fHead->CalcTextSizeA(fs, FLT_MAX, 0.f, title);
  const ImVec2 tp(p.x + g.S(22), p.y - ts.y * 0.55f);
  dl->AddRectFilled(ImVec2(tp.x - g.S(8), tp.y), ImVec2(tp.x + ts.x + g.S(8), tp.y + ts.y), kBg);
  dl->AddText(g.fHead, fs, tp, kText, title);
}

void Label(ImDrawList* dl, const char* text, ImVec2 p) { dl->AddText(g.fBody, g.szBody, p, kTextDim, text); }

bool Combo(const char* id, const std::vector<std::string>& labels, int& idx, float w) {
  bool changed = false;
  ImGui::SetNextItemWidth(w);
  if (ImGui::BeginCombo(id, labels[(size_t)idx].c_str())) {
    for (int i = 0; i < (int)labels.size(); i++) {
      ImGui::PushID(i);
      if (ImGui::Selectable(labels[(size_t)i].c_str(), i == idx)) {
        idx = i;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Medidor segmentado. `peak` es lineal (0..1).
void DrawMeter(ImDrawList* dl, ImVec2 p, float w, float h, Meter& m, float peak, float dt) {
  const float db = peak > 1e-5f ? 20.f * std::log10(peak) : -100.f;
  const float n = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
  m.lvl = std::max(n, m.lvl - dt * 1.4f);
  if (n >= m.hold) {
    m.hold = n;
    m.holdT = 0.7f;
  } else {
    m.holdT -= dt;
    if (m.holdT <= 0.f) m.hold = std::max(n, m.hold - dt * 0.7f);
  }

  const int segs = 34;
  const float gap = g.S(2.f);
  const float sw = (w - gap * (segs - 1)) / segs;
  const int holdIdx = std::clamp((int)(m.hold * segs), 0, segs - 1);
  for (int i = 0; i < segs; i++) {
    const float t = (i + 0.5f) / segs;
    static const ImU32 stops[6] = {IM_COL32(40, 220, 245, 255), IM_COL32(70, 235, 120, 255), IM_COL32(245, 225, 70, 255),
                                   IM_COL32(255, 150, 60, 255),  IM_COL32(240, 80, 190, 255), IM_COL32(150, 90, 250, 255)};
    const float ft = std::clamp(t, 0.f, 0.999f) * 5.f;
    const ImU32 col = Lerp32(stops[(int)ft], stops[(int)ft + 1], ft - (int)ft);
    const bool on = t <= m.lvl;
    ImU32 c = on ? col : ((col & 0x00FFFFFF) | (80u << 24));
    if (!on && i == holdIdx && m.hold > 0.02f) c = kSegWarn;
    const float x0 = p.x + i * (sw + gap);
    dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + sw, p.y + h), c, g.S(1.5f));
  }
}

bool NeonButton(const char* label, ImVec2 size, bool selected = false, bool disabled = false) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, g.S(selected ? 2.5f : 1.5f));
  ImGui::PushStyleColor(ImGuiCol_Border, selected ? kMagenta : IM_COL32(165, 95, 235, 255));
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? IM_COL32(70, 28, 90, 255) : IM_COL32(17, 22, 36, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 36, 60, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(44, 50, 84, 255));
  if (disabled) ImGui::BeginDisabled();
  const bool r = ImGui::Button(label, size);
  if (disabled) ImGui::EndDisabled();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar();
  return r;
}

void TitleButtons(ImDrawList* dl) {
  const float sz = g.S(34);
  const float y = g.S(26);
  const float xClose = g.S(kWinW) - g.S(24) - sz;
  const float xMin = xClose - sz - g.S(10);

  auto hit = [&](const char* id, float x) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::InvisibleButton(id, ImVec2(sz, sz));
    return std::make_pair(ImGui::IsItemClicked(), ImGui::IsItemHovered());
  };

  auto [minClick, minHover] = hit("##tmin", xMin);
  auto [closeClick, closeHover] = hit("##tclose", xClose);

  const ImU32 cMin = minHover ? kCyan : kTextDim;
  const ImU32 cClose = closeHover ? kMagenta : kTextDim;
  const float th = g.S(2.f);
  dl->AddLine(ImVec2(xMin + g.S(8), y + sz * 0.5f), ImVec2(xMin + sz - g.S(8), y + sz * 0.5f), cMin, th);
  const float a = g.S(10);
  dl->AddLine(ImVec2(xClose + a, y + a), ImVec2(xClose + sz - a, y + sz - a), cClose, th);
  dl->AddLine(ImVec2(xClose + sz - a, y + a), ImVec2(xClose + a, y + sz - a), cClose, th);

  if (minClick) ShowWindow(g.hwnd, SW_MINIMIZE);
  if (closeClick) PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

void DrawOptions() {
  if (g.showOptions) {
    ImGui::OpenPopup("Options");
    CopyStr(g.outDirBuf, sizeof(g.outDirBuf), WideToUtf8(g.st.outputDir));
    g.showOptions = false;
  }

  ImGui::SetNextWindowPos(ImVec2(g.S(kWinW) * 0.5f, g.S(kWinH) * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(g.S(560), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, g.S(16));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, g.S(2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(g.S(26), g.S(18)));
  ImGui::PushStyleColor(ImGuiCol_Border, kPanelBorder);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(13, 17, 28, 255));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
  const bool open = ImGui::BeginPopupModal("Options", nullptr,
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  if (!open) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(g.fHead, g.szHead, ImGui::GetCursorScreenPos(), kCyan, "Options");
  ImGui::Dummy(ImVec2(0, g.szHead + g.S(10)));

  ImGui::TextColored(ImColor(kTextDim), "Save recordings to");
  ImGui::SetNextItemWidth(g.S(400));
  if (ImGui::InputText("##dir", g.outDirBuf, sizeof(g.outDirBuf))) {
    g.st.outputDir = Utf8ToWide(g.outDirBuf);
    g.dirty = true;
  }
  ImGui::SameLine();
  if (NeonButton("Browse...", ImVec2(g.S(104), 0))) g.wantBrowse = true;

  ImGui::Dummy(ImVec2(0, g.S(4)));

  // Formato, FPS y calidad en una sola fila para que el cuadro quepa en la ventana.
  const float colGap = g.S(14);
  int fmt = g.st.mp4 ? 0 : 1;
  ImGui::BeginGroup();
  ImGui::TextColored(ImColor(kTextDim), "Format");
  if (Combo("##fmt", {"MP4 (recommended)", "MKV (safer)"}, fmt, g.S(190))) {
    g.st.mp4 = fmt == 0;
    g.dirty = true;
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, colGap);
  int fpsIdx = g.st.fps >= 60 ? 2 : (g.st.fps <= 24 ? 0 : 1);
  ImGui::BeginGroup();
  ImGui::TextColored(ImColor(kTextDim), "Frame rate");
  if (Combo("##fps", {"24 fps", "30 fps", "60 fps"}, fpsIdx, g.S(130))) {
    g.st.fps = fpsIdx == 0 ? 24 : (fpsIdx == 1 ? 30 : 60);
    g.dirty = true;
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, colGap);
  ImGui::BeginGroup();
  ImGui::TextColored(ImColor(kTextDim), "Quality");
  if (Combo("##q", {"Low (small files)", "Medium", "High"}, g.st.quality, g.S(140))) g.dirty = true;
  ImGui::EndGroup();

  ImGui::Dummy(ImVec2(0, g.S(2)));
  if (ImGui::Checkbox("Record the mouse cursor", &g.st.cursor)) g.dirty = true;
  if (g.st.cursor) {
    // Tamaño y color del cursor SOLO en el video: no cambian el cursor real de Windows.
    ImGui::BeginGroup();
    ImGui::TextColored(ImColor(kTextDim), "Cursor size in the video");
    int sz = g.st.cursorScalePct >= 300 ? 3 : (g.st.cursorScalePct >= 200 ? 2 : (g.st.cursorScalePct >= 150 ? 1 : 0));
    if (Combo("##csz", {"Normal", "Large (150%)", "Extra large (200%)", "Huge (300%)"}, sz, g.S(190))) {
      static const int pct[] = {100, 150, 200, 300};
      g.st.cursorScalePct = pct[sz];
      g.dirty = true;
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, colGap);
    ImGui::BeginGroup();
    ImGui::TextColored(ImColor(kTextDim), "Cursor color in the video");
    const float r = g.S(11), gap = g.S(5);
    ImGui::Dummy(ImVec2(0, g.S(6)));
    for (int i = 0; i < kCursorColorCount; i++) {
      if (i) ImGui::SameLine(0, gap);
      ImGui::PushID(1000 + i);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##cc", ImVec2(r * 2, r * 2));
      const bool hv = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) {
        g.st.cursorColor = i;
        g.dirty = true;
      }
      ImDrawList* odl = ImGui::GetWindowDrawList();
      const ImVec2 c(p0.x + r, p0.y + r);
      const int rgb = kCursorColors[i].rgb;
      if (rgb < 0) {  // "Original": mitad blanca y mitad negra, como el cursor de Windows
        odl->AddCircleFilled(c, r - g.S(2), IM_COL32(20, 20, 20, 255));
        odl->PathClear();
        odl->PathArcTo(c, r - g.S(2), -1.5708f, 1.5708f, 16);
        odl->PathFillConvex(IM_COL32(245, 245, 245, 255));
      } else {
        odl->AddCircleFilled(c, r - g.S(2), IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255));
      }
      odl->AddCircle(c, r - g.S(2), IM_COL32(120, 135, 200, 255), 0, g.S(1.2f));
      if (g.st.cursorColor == i) odl->AddCircle(c, r + g.S(0.5f), kCyan, 0, g.S(2.2f));
      if (hv) ImGui::SetTooltip("%s", kCursorColors[i].name);
      ImGui::PopID();
    }
    ImGui::EndGroup();
  }
  if (ImGui::Checkbox("Desktop and microphone as separate audio tracks", &g.st.separateTracks)) g.dirty = true;
  if (ImGui::Checkbox("Camera photos at maximum resolution (slower: 3-4 s)", &g.st.photoMax)) g.dirty = true;

  ImGui::Dummy(ImVec2(0, g.S(6)));
  if (NeonButton("Done", ImVec2(g.S(120), g.S(38)))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}


// ───────────────────────────── Acerca de ────────────────────────────────
std::string WindowsVersionText() {
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  OSVERSIONINFOW vi{sizeof(vi)};
  if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&vi);
  char buf[96];
  snprintf(buf, sizeof buf, "Windows %s (build %lu)", vi.dwBuildNumber >= 22000 ? "11" : "10", vi.dwBuildNumber);
  return buf;
}

// __DATE__ ("Nov  1 2026") -> "01/11/2026"
std::string BuildDateText() {
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* d = __DATE__;
  int m = 1;
  for (int i = 0; i < 12; i++)
    if (!strncmp(d, months + i * 3, 3)) m = i + 1;
  char buf[32];
  snprintf(buf, sizeof buf, "%02d/%02d/%s", atoi(d + 4), m, d + 7);
  return buf;
}

std::string AboutInfoText() {
  std::string t = "Miau Screen Recorder\r\n";
  t += "Version " MIAU_VERSION " - built on " + BuildDateText() + "\r\n";
  t += "Made by: IA21\r\nWebsite: ia21.cc\r\n";
  t += std::string("Media engine: FFmpeg ") + av_version_info() + "\r\n";
  t += std::string("FFmpeg license: ") + avutil_license() + "\r\n";
  t += "Formats: MP4 / MKV (H.264 or MPEG-4 video, AAC audio)\r\n";
  t += "System: " + WindowsVersionText() + "\r\n";
  return t;
}

void CopyToClipboard(const std::string& utf8) {
  const std::wstring w = Utf8ToWide(utf8);
  if (!OpenClipboard(g.hwnd)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
    memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
}

void DrawAbout() {
  if (g.showAbout) {
    ImGui::OpenPopup("About");
    g.showAbout = false;
  }

  ImGui::SetNextWindowPos(ImVec2(g.S(kWinW) * 0.5f, g.S(kWinH) * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(g.S(560), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, g.S(16));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, g.S(2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(g.S(28), g.S(20)));
  ImGui::PushStyleColor(ImGuiCol_Border, kPanelBorder);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(13, 17, 28, 255));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 140));
  const bool open = ImGui::BeginPopupModal("About", nullptr,
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  if (!open) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float W = ImGui::GetContentRegionAvail().x;
  const ImU32 kRed = IM_COL32(255, 96, 96, 255);

  // Cabecera: ícono + nombre + versión + descripción
  const float ic = g.S(72);
  dl->AddImage(TexId(g.logo), p0, ImVec2(p0.x + ic, p0.y + ic));
  const float tx = p0.x + ic + g.S(18);
  dl->AddText(g.fTitle, g.szTitle, ImVec2(tx, p0.y - g.S(6)), kCyan, "Miau Screen Recorder");
  const std::string ver = std::string("Version " MIAU_VERSION "  ·  built on ") + BuildDateText();
  dl->AddText(g.fBody, g.szBody, ImVec2(tx, p0.y + g.S(34)), kRed, ver.c_str());
  dl->AddText(g.fBody, g.szBody, ImVec2(tx, p0.y + g.S(56)), kTextDim, "Lightweight screen recorder");
  ImGui::Dummy(ImVec2(0, ic + g.S(12)));

  // Filas de datos
  auto row = [&](const char* k, const std::string& v, bool link = false, const char* url = nullptr) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(g.fBody, g.szBody, p, kTextDim, k);
    const ImVec2 vp(p.x + g.S(160), p.y);
    dl->AddText(g.fBody, g.szBody, vp, link ? kRed : kText, v.c_str());
    if (link && url) {
      const ImVec2 ts = g.fBody->CalcTextSizeA(g.szBody, FLT_MAX, 0.f, v.c_str());
      ImGui::SetCursorScreenPos(vp);
      ImGui::InvisibleButton(k, ts);
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddLine(ImVec2(vp.x, vp.y + ts.y), ImVec2(vp.x + ts.x, vp.y + ts.y), kRed, g.S(1.f));
      }
      if (ImGui::IsItemClicked()) ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
      ImGui::SetCursorScreenPos(p);
    }
    ImGui::Dummy(ImVec2(0, g.szBody + g.S(7)));
  };
  row("Made by", "IA21");
  row("Website", "ia21.cc", true, "https://ia21.cc");
  row("Media engine", std::string("FFmpeg ") + av_version_info());
  row("FFmpeg license", avutil_license());
  row("Formats", "MP4 / MKV  (H.264 or MPEG-4, AAC)");
  row("System", WindowsVersionText());

  // Botón Ko-fi
  ImGui::Dummy(ImVec2(0, g.S(2)));
  {
    const ImVec2 k0 = ImGui::GetCursorScreenPos();
    const float kh = g.S(48);
    ImGui::InvisibleButton("##kofi", ImVec2(W, kh));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddRectFilled(k0, ImVec2(k0.x + W, k0.y + kh), hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(244, 246, 252, 255),
                      g.S(14));
    // Taza de café
    const ImVec2 c(k0.x + g.S(34), k0.y + kh / 2);
    const ImU32 coffee = IM_COL32(255, 94, 91, 255);
    dl->AddRectFilled(ImVec2(c.x - g.S(13), c.y - g.S(9)), ImVec2(c.x + g.S(9), c.y + g.S(10)), coffee, g.S(5));
    dl->AddCircle(ImVec2(c.x + g.S(11), c.y - g.S(1)), g.S(5), coffee, 0, g.S(2.5f));
    dl->AddCircleFilled(ImVec2(c.x - g.S(2), c.y), g.S(4.5f), IM_COL32(255, 255, 255, 255));
    const char* kt = "If you like Miau Screen Recorder, buy me a coffee";
    const ImVec2 kts = g.fHead->CalcTextSizeA(g.szBody * 1.05f, FLT_MAX, 0.f, kt);
    dl->AddText(g.fHead, g.szBody * 1.05f, ImVec2(k0.x + g.S(64), k0.y + (kh - kts.y) / 2), IM_COL32(20, 22, 50, 255), kt);
    if (ImGui::IsItemClicked()) ShellExecuteA(nullptr, "open", "https://ko-fi.com/miauapps", nullptr, nullptr, SW_SHOWNORMAL);
  }

  ImGui::Dummy(ImVec2(0, g.S(6)));
  dl->AddText(g.fBody, g.szBody * 0.9f, ImGui::GetCursorScreenPos(), kTextDim,
              "Uses FFmpeg (ffmpeg.org). Provided without warranty of any kind.");
  ImGui::Dummy(ImVec2(0, g.szBody + g.S(8)));

  if (NeonButton("Copy info", ImVec2(g.S(160), g.S(38)))) CopyToClipboard(AboutInfoText());
  ImGui::SameLine(W + g.S(28) - g.S(160));
  if (NeonButton("Close", ImVec2(g.S(160), g.S(38)))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ─────────────────────────────── La interfaz ────────────────────────────
void DrawUI() {
  ImGuiIO& io = ImGui::GetIO();
  const float W = g.S(kWinW), H = g.S(kWinH);
  const bool idle = g.mode == App::Mode::Idle && !g.camRecording && !g.camSaving;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::Begin("##main", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Fondo y borde neón
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), kBg, g.S(10));
  GradRect(dl, ImVec2(1, 1), ImVec2(W - 1, H - 1), kBorder, kMagenta, g.S(10), g.S(2.5f));

  // Título
  dl->AddImage(TexId(g.logo), ImVec2(g.S(30), g.S(16)), ImVec2(g.S(30) + g.S(56), g.S(16) + g.S(56)));
  dl->AddText(g.fTitle, g.szTitle, ImVec2(g.S(100), g.S(18)), kCyan, "Miau Screen Recorder");
  TitleButtons(dl);

  // ── Panel de audio ──
  const float py = g.S(94), ph = g.S(340);
  const float lx = g.S(26), lw = g.S(400);
  const float rx = lx + lw + g.S(18), rw = W - rx - g.S(26);
  DrawPanel(dl, "Audio", ImVec2(lx, py), ImVec2(lw, ph));

  const float ix = lx + g.S(22), iw = lw - g.S(44);
  float y = py + g.S(38);
  Label(dl, "Desktop audio (what you hear)", ImVec2(ix, y));
  ImGui::SetCursorPos(ImVec2(ix, y + g.S(24)));
  int di = IndexOf(g.dIds, g.st.desktopId, 1);
  if (Combo("##desk", g.dLabels, di, iw) && idle) {
    g.st.desktopId = g.dIds[(size_t)di];
    g.dirty = true;
  }
  DrawMeter(dl, ImVec2(ix, y + g.S(72)), iw, g.S(16), g.mDesktop,
            std::max(g.desktop.PeakLeft(), g.desktop.PeakRight()), io.DeltaTime);

  y += g.S(128);
  Label(dl, "Microphone input", ImVec2(ix, y));
  ImGui::SetCursorPos(ImVec2(ix, y + g.S(24)));
  int mi = IndexOf(g.mIds, g.st.micId, 1);
  if (Combo("##mic", g.mLabels, mi, iw) && idle) {
    g.st.micId = g.mIds[(size_t)mi];
    g.dirty = true;
  }
  DrawMeter(dl, ImVec2(ix, y + g.S(72)), iw, g.S(16), g.mMic, std::max(g.mic.PeakLeft(), g.mic.PeakRight()),
            io.DeltaTime);

  Label(dl, "Choose the output you listen with.", ImVec2(ix, py + ph - g.S(34)));

  // ── Panel de video / cámara ──
  DrawPanel(dl, "Video / Camera", ImVec2(rx, py), ImVec2(rw, ph));
  const float cx = rx + g.S(22), cwid = rw - g.S(44);
  y = py + g.S(38);
  Label(dl, "Camera source", ImVec2(cx, y));
  ImGui::SetCursorPos(ImVec2(cx, y + g.S(24)));
  int ci = IndexOf(g.cIds, g.st.cameraLink, 0);
  if (Combo("##cam", g.cLabels, ci, cwid) && idle) {
    g.st.cameraLink = g.cIds[(size_t)ci];
    g.dirty = true;
  }

  // Vista previa de la cámara
  y += g.S(72);
  Label(dl, "Camera preview", ImVec2(cx, y));
  const ImVec2 pv0(cx, y + g.S(26));
  const ImVec2 pv1(cx + cwid, y + g.S(26) + g.S(190));
  const float pw = pv1.x - pv0.x, phh = pv1.y - pv0.y;
  if (g.camTex.srv) {
    const float ra = pw / phh, ca = (float)g.camTex.w / (float)g.camTex.h;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ca > ra) {
      const float k = ra / ca;
      uv0.x = (1 - k) / 2;
      uv1.x = 1 - uv0.x;
    } else {
      const float k = ca / ra;
      uv0.y = (1 - k) / 2;
      uv1.y = 1 - uv0.y;
    }
    dl->AddImageRounded(TexId(g.camTex), pv0, pv1, uv0, uv1, IM_COL32_WHITE, g.S(20));
  } else {
    dl->AddRectFilled(pv0, pv1, IM_COL32(16, 20, 32, 255), g.S(20));
    char startBuf[160];
    snprintf(startBuf, sizeof startBuf, "Starting camera... (try %d, mode %d, %s, err 0x%08lX)", g.camera.Attempt(),
             g.camera.Mode(), g.camera.Stage(), (unsigned long)g.camera.LastHr());
    const char* msg = g.cams.empty() ? "No camera found" : (CamOff() ? "No camera selected" : startBuf);
    const ImVec2 ts = g.fBody->CalcTextSizeA(g.szBody, FLT_MAX, 0.f, msg);
    dl->AddText(g.fBody, g.szBody, ImVec2(pv0.x + (pw - ts.x) / 2, pv0.y + (phh - ts.y) / 2), kTextDim, msg);
  }
  for (int i = 3; i >= 1; i--)  // resplandor rosa
    dl->AddRect(pv0 - ImVec2(g.S(1.5f * i), g.S(1.5f * i)), pv1 + ImVec2(g.S(1.5f * i), g.S(1.5f * i)),
                IM_COL32(214, 100, 240, 20), g.S(20 + 1.5f * i), 0, g.S(1.5f));
  GradRect(dl, pv0, pv1, kMagenta, kViolet, g.S(20), g.S(2.f));

  // Avisos sobre la vista previa
  {
    const char* badge = nullptr;
    bool recDot = false;
    if (g.camSaving) badge = "Saving video...";
    else if (g.camRecording) { badge = "Capturing camera video..."; recDot = true; }
    else if (g.photoOverlay) badge = "Saving image...";
    if (badge) {
      const float fs = g.szBody;
      const ImVec2 ts = g.fHead->CalcTextSizeA(fs, FLT_MAX, 0.f, badge);
      const float padX = g.S(14), dotW = recDot ? g.S(20) : 0.f, h = ts.y + g.S(12);
      const float w = ts.x + padX * 2 + dotW;
      // Abajo, pegado al borde de la vista previa, para no tapar la imagen.
      const ImVec2 c((pv0.x + pv1.x) / 2, pv1.y - g.S(8) - h / 2);
      const ImVec2 b0(c.x - w / 2, c.y - h / 2), b1(c.x + w / 2, c.y + h / 2);
      dl->AddRectFilled(b0, b1, IM_COL32(6, 10, 20, 215), h / 2);
      dl->AddRect(b0, b1, IM_COL32(94, 242, 236, 160), h / 2, 0, g.S(1.5f));
      float tx = b0.x + padX;
      if (recDot) {
        const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 5.f);
        dl->AddCircleFilled(ImVec2(tx + g.S(6), c.y), g.S(6), IM_COL32(255, 60, 60, (int)(255 * pulse)));
        tx += dotW;
      }
      dl->AddText(g.fHead, fs, ImVec2(tx, c.y - ts.y / 2), IM_COL32(240, 248, 255, 255), badge);
    }
  }

  // ── Fila inferior ──
  const float by = py + ph + g.S(14);
  ImGui::SetCursorPos(ImVec2(lx, by + g.S(4)));
  const float bs = g.S(54), bgap = g.S(12);
  if (NeonButton("##options", ImVec2(bs, bs), false, !idle)) g.showOptions = true;
  {  // ícono de engranaje
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    const ImVec2 c((a0.x + a1.x) / 2, (a0.y + a1.y) / 2);
    const bool hv = ImGui::IsItemHovered();
    const ImU32 col = !idle ? kTextDim : (hv ? kCyan : kText);
    for (int i = 0; i < 8; i++) {
      const float an = (float)i * 3.14159265f / 4.f;
      const ImVec2 d(std::cos(an), std::sin(an));
      dl->AddLine(ImVec2(c.x + d.x * g.S(9), c.y + d.y * g.S(9)), ImVec2(c.x + d.x * g.S(13), c.y + d.y * g.S(13)), col,
                  g.S(4.f));
    }
    dl->AddCircle(c, g.S(9), col, 0, g.S(2.4f));
    dl->AddCircle(c, g.S(3.6f), col, 0, g.S(2.f));
    if (hv) ImGui::SetTooltip("Options");
  }

  // Botón "Acerca de" (ícono "i")
  ImGui::SetCursorPos(ImVec2(lx + bs + bgap, by + g.S(4)));
  if (NeonButton("##about", ImVec2(bs, bs), false, !idle)) g.showAbout = true;
  {
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    const ImVec2 c((a0.x + a1.x) / 2, (a0.y + a1.y) / 2);
    const ImU32 col = ImGui::IsItemHovered() ? kCyan : kText;
    dl->AddCircle(c, g.S(14), col, 0, g.S(2.f));
    dl->AddCircleFilled(ImVec2(c.x, c.y - g.S(6)), g.S(2.2f), col);
    dl->AddLine(ImVec2(c.x, c.y - g.S(1.5f)), ImVec2(c.x, c.y + g.S(7)), col, g.S(2.6f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("About");
  }

  // Botón "Show in folder" (ícono de carpeta): activo cuando hay una grabación guardada.
  ImGui::SetCursorPos(ImVec2(W - g.S(26) - bs * 3 - bgap * 2, by + g.S(4)));
  // Siempre activo: con una grabación reciente abre su carpeta con el archivo seleccionado;
  // si no, abre la carpeta de salida configurada.
  const bool canOpen = true;
  if (NeonButton("##folder", ImVec2(bs, bs))) {
    if (!g.lastFile.empty() && !g.statusError && GetFileAttributesW(g.lastFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
      const std::wstring arg = L"/select,\"" + g.lastFile + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
      CreateDirectoryW(g.st.outputDir.c_str(), nullptr);  // por si aún no existe
      ShellExecuteW(nullptr, L"open", g.st.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
  }
  {
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    const ImVec2 c((a0.x + a1.x) / 2, (a0.y + a1.y) / 2);
    const bool hv = ImGui::IsItemHovered();
    const ImU32 col = !canOpen ? IM_COL32(110, 122, 165, 255) : (hv ? kCyan : kText);
    const float t = g.S(2.4f);
    dl->AddRect(ImVec2(c.x - g.S(13), c.y - g.S(7)), ImVec2(c.x + g.S(13), c.y + g.S(10)), col, g.S(3), 0, t);
    const ImVec2 tab[4] = {ImVec2(c.x - g.S(13), c.y - g.S(7)), ImVec2(c.x - g.S(13), c.y - g.S(11)),
                           ImVec2(c.x - g.S(4), c.y - g.S(11)), ImVec2(c.x - g.S(1), c.y - g.S(7))};
    dl->AddPolyline(tab, 4, col, 0, t);
    if (hv && canOpen) ImGui::SetTooltip("Show in folder");
  }

  // Botón de grabar
  const float sw = g.S(280), sh = g.S(62), sx = (W - sw) / 2;
  const ImVec2 s0(sx, by - g.S(4)), s1(sx + sw, by - g.S(4) + sh);
  ImGui::SetCursorPos(s0);
  ImGui::InvisibleButton("##start", ImVec2(sw, sh));
  const bool hov = ImGui::IsItemHovered() && idle;
  const bool startClick = ImGui::IsItemClicked() && idle;
  if (idle) {
    for (int i = 3; i >= 1; i--)  // resplandor
      dl->AddRectFilled(s0 - ImVec2(g.S(3.f * i), g.S(3.f * i)), s1 + ImVec2(g.S(3.f * i), g.S(3.f * i)),
                        IM_COL32(214, 100, 240, 22), g.S(22 + 3.f * i));
  }
  const ImU32 fill = !idle ? IM_COL32(34, 42, 58, 255) : (hov ? IM_COL32(18, 100, 112, 255) : IM_COL32(12, 74, 86, 255));
  dl->AddRectFilled(s0, s1, fill, g.S(22));
  GradRect(dl, s0, s1, idle ? kCyan : IM_COL32(80, 96, 120, 255), idle ? IM_COL32(110, 245, 235, 255) : IM_COL32(80, 96, 120, 255),
           g.S(22), g.S(2.f));
  const char* startTxt = g.mode == App::Mode::Saving ? "SAVING..." : (g.mode == App::Mode::Recording ? "RECORDING" : "START RECORDING");
  const ImVec2 sts2 = g.fHead->CalcTextSizeA(g.szHead, FLT_MAX, 0.f, startTxt);
  const float iconR = g.S(13);
  const float totalW = iconR * 2 + g.S(14) + sts2.x;
  const float tx = sx + (sw - totalW) / 2;
  const ImU32 ink = idle ? IM_COL32(235, 250, 252, 255) : IM_COL32(150, 165, 190, 255);
  dl->AddCircle(ImVec2(tx + iconR, by - g.S(4) + sh / 2), iconR, ink, 0, g.S(3));
  dl->AddCircleFilled(ImVec2(tx + iconR, by - g.S(4) + sh / 2), iconR * 0.55f, ink);
  dl->AddText(g.fHead, g.szHead, ImVec2(tx + iconR * 2 + g.S(14), by - g.S(4) + (sh - sts2.y) / 2), ink, startTxt);
  if (startClick) g.wantStart = true;

  // Botón de foto (ícono de cámara): toma una foto con la máxima resolución de la cámara.
  ImGui::SetCursorPos(ImVec2(W - g.S(26) - bs * 2 - bgap, by + g.S(4)));
  const bool canPhoto = idle && !CamOff() && g.camera.Running() && !g.photoBusy;
  if (NeonButton("##photo", ImVec2(bs, bs), false, !canPhoto)) StartPhoto();
  {
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    const ImVec2 c((a0.x + a1.x) / 2, (a0.y + a1.y) / 2);
    const bool hv = ImGui::IsItemHovered();
    const ImU32 col = !canPhoto ? IM_COL32(110, 122, 165, 255) : (hv ? kCyan : kText);
    const float t = g.S(2.4f);
    dl->AddRect(ImVec2(c.x - g.S(14), c.y - g.S(8)), ImVec2(c.x + g.S(14), c.y + g.S(11)), col, g.S(4), 0, t);
    const ImVec2 bump[4] = {ImVec2(c.x - g.S(6), c.y - g.S(8)), ImVec2(c.x - g.S(4), c.y - g.S(12)),
                            ImVec2(c.x + g.S(4), c.y - g.S(12)), ImVec2(c.x + g.S(6), c.y - g.S(8))};
    dl->AddPolyline(bump, 4, col, 0, t);
    dl->AddCircle(ImVec2(c.x, c.y + g.S(1.5f)), g.S(5.5f), col, 0, t);
    if (hv) ImGui::SetTooltip(canPhoto ? "Take a photo with the camera" : "Photo needs a working camera");
  }

  // Botón de video de la cámara: cuadro con un punto rojo grueso. Graba cámara + micrófono seleccionado.
  ImGui::SetCursorPos(ImVec2(W - g.S(26) - bs, by + g.S(4)));
  const bool camOk = !CamOff() && g.camera.Running();
  const bool canVideo = g.camRecording || (idle && camOk && !g.photoBusy);
  if (NeonButton("##camvideo", ImVec2(bs, bs), g.camRecording, !canVideo)) {
    if (g.camRecording) StopCameraVideo();
    else StartCameraVideo();
  }
  {
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    const ImVec2 c((a0.x + a1.x) / 2, (a0.y + a1.y) / 2);
    const bool hv = ImGui::IsItemHovered();
    const ImU32 col = !canVideo ? IM_COL32(110, 122, 165, 255) : (g.camRecording ? IM_COL32(255, 90, 90, 255) : (hv ? kCyan : kText));
    const float t = g.S(2.4f);
    dl->AddRect(ImVec2(c.x - g.S(14), c.y - g.S(11)), ImVec2(c.x + g.S(14), c.y + g.S(11)), col, g.S(5), 0, t);
    if (g.camRecording) {  // detener: cuadrito
      dl->AddRectFilled(ImVec2(c.x - g.S(5.5f), c.y - g.S(5.5f)), ImVec2(c.x + g.S(5.5f), c.y + g.S(5.5f)), IM_COL32(255, 90, 90, 255), g.S(2));
    } else {  // punto rojo grueso
      dl->AddCircleFilled(c, g.S(6.5f), canVideo ? IM_COL32(255, 52, 52, 255) : IM_COL32(150, 70, 80, 255));
    }
    if (hv) ImGui::SetTooltip(g.camRecording ? "Stop camera video" : (canVideo ? "Record camera video (with microphone)" : "Video needs a working camera"));
  }

  // Estado: una línea bajo los botones.
  if (!g.status.empty()) {
    dl->AddText(g.fBody, g.szBody, ImVec2(lx, by + g.S(59)), g.statusError ? IM_COL32(255, 120, 130, 255) : kCyan,
                g.status.c_str());
  }

  DrawOptions();
  DrawAbout();
  ImGui::End();
}

void SetupStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.FrameRounding = g.S(10);
  s.PopupRounding = g.S(10);
  s.GrabRounding = g.S(8);
  s.FramePadding = ImVec2(g.S(12), g.S(8));
  s.ItemSpacing = ImVec2(g.S(10), g.S(8));
  s.FrameBorderSize = g.S(1.5f);
  s.PopupBorderSize = g.S(1.5f);
  s.WindowPadding = ImVec2(g.S(10), g.S(10));
  s.GrabMinSize = g.S(18);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImColor(kText);
  c[ImGuiCol_Border] = ImColor(kPanelBorder);
  c[ImGuiCol_FrameBg] = ImColor(IM_COL32(14, 22, 34, 255));
  c[ImGuiCol_FrameBgHovered] = ImColor(IM_COL32(20, 32, 48, 255));
  c[ImGuiCol_FrameBgActive] = ImColor(IM_COL32(26, 42, 62, 255));
  c[ImGuiCol_PopupBg] = ImColor(IM_COL32(13, 17, 28, 250));
  c[ImGuiCol_Header] = ImColor(IM_COL32(24, 66, 92, 255));
  c[ImGuiCol_HeaderHovered] = ImColor(IM_COL32(30, 90, 118, 255));
  c[ImGuiCol_HeaderActive] = ImColor(IM_COL32(38, 110, 138, 255));
  c[ImGuiCol_SliderGrab] = ImColor(kCyan);
  c[ImGuiCol_SliderGrabActive] = ImColor(IM_COL32(150, 255, 248, 255));
  c[ImGuiCol_CheckMark] = ImColor(kCyan);
  c[ImGuiCol_Button] = ImColor(IM_COL32(17, 22, 36, 255));
}

bool FileExists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

ImFont* LoadFont(const wchar_t* wpath, const char* path, float px) {
  ImGuiIO& io = ImGui::GetIO();
  ImFont* f = FileExists(wpath) ? io.Fonts->AddFontFromFileTTF(path, px) : nullptr;
  return f ? f : io.Fonts->AddFontDefault();
}

// ───────────────────────────── Ventana Win32 ────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg != WM_NCHITTEST && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

  switch (msg) {
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      RECT rc;
      GetClientRect(hwnd, &rc);
      // La franja del título arrastra la ventana (menos los botones - y X).
      if (pt.y >= 0 && pt.y < (int)g.S(kTitleH) && pt.x >= 0 && pt.x < rc.right - (int)g.S(110)) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && g.swap) ResizeTarget();
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // sin menú con Alt
      break;
    case WM_DEVICECHANGE:
      g.wantRefresh = true;
      return TRUE;
    case WM_ACTIVATEAPP:
      if (wp) g.wantRefresh = true;
      break;
    case WM_APP_STOP:
      g.wantStop = true;
      return 0;
    case WM_APP_FINISHED:
      g.finished = true;
      return 0;
    case WM_APP_PHOTO:
      FinishPhoto();
      return 0;
    case WM_APP_CAMDONE:
      FinishCameraVideo();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ───────────────────────────────── main ─────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  // Coordenadas en píxeles físicos en todos los monitores (lo necesita el selector de área).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Reinicio tras una grabación: esperar a que termine la instancia anterior.
  bool restarted = false;
  int restartX = 0, restartY = 0;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 5 && std::wstring(argv[1]) == L"--restart") {
      restarted = true;
      const DWORD pid = (DWORD)_wtoi(argv[2]);
      restartX = _wtoi(argv[3]);
      restartY = _wtoi(argv[4]);
      if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 15000);
        CloseHandle(h);
      }
    }
    if (argv) LocalFree(argv);
  }

  const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MiauScreenRecorder.Single");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Miau Screen Recorder is already running.", L"Miau Screen Recorder", MB_ICONINFORMATION);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  MFStartup(MF_VERSION);

  LogReset();
  RestoreSystemCursors(CursorMarkerPath());  // por si una sesión anterior se cerró sin restaurar los cursores
  g.hInst = hInst;
  g.scale = (float)GetDpiForSystem() / 96.f;
  {  // En pantallas pequeñas (p. ej. 1024x768 con escala 125 %) la ventana se reduce para que quepa entera.
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
      const float fit = std::min(0.96f * (float)(wa.right - wa.left) / kWinW, 0.96f * (float)(wa.bottom - wa.top) / kWinH);
      if (fit > 0.3f && g.scale > fit) g.scale = fit;
    }
  }
  g.st.Load();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = L"MiauScreenRecorderMain";
  RegisterClassExW(&wc);

  const int ww = (int)g.S(kWinW), wh = (int)g.S(kWinH);
  int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
  if (restarted) {  // misma posición que tenía antes de reiniciar
    sx = restartX;
    sy = restartY;
  }
  g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Miau Screen Recorder",
                           WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
  if (!g.hwnd) return 1;

  const int cornerRound = 2;  // DWMWCP_ROUND (Windows 11; en Windows 10 se ignora)
  DwmSetWindowAttribute(g.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerRound, sizeof(cornerRound));

  if (!CreateDevice()) {
    MessageBoxW(nullptr, L"Could not initialize Direct3D 11.", L"Miau Screen Recorder", MB_ICONERROR);
    return 1;
  }
  CreateTarget();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;

  g.szBody = g.S(17);
  g.szHead = g.S(25);
  g.szTitle = g.S(40);
  g.fBody = LoadFont(L"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", g.szBody);
  g.fHead = LoadFont(L"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\seguisb.ttf", g.szHead);
  g.fTitle = LoadFont(L"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\seguisb.ttf", g.szTitle);
  SetupStyle();

  ImGui_ImplWin32_Init(g.hwnd);
  ImGui_ImplDX11_Init(g.dev.Get(), g.ctx.Get());

  MakeTexture(g.logo, kLogoRGBA, kLogoSize, kLogoSize, DXGI_FORMAT_R8G8B8A8_UNORM);

  if (restarted) {  // recupera el aviso de la grabación anterior ("Saved: ...")
    FILE* f = nullptr;
    _wfopen_s(&f, RestartFile().c_str(), L"rb");
    if (f) {
      char buf[2048] = {};
      const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      DeleteFileW(RestartFile().c_str());
      std::string txt(buf, n);
      const size_t a = txt.find('\n'), b = a == std::string::npos ? a : txt.find('\n', a + 1);
      if (b != std::string::npos) {
        std::string msg = txt.substr(b + 1);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        std::string path = txt.substr(a + 1, b - a - 1);
        const bool err = txt[0] == '1';
        SetStatus(msg, err);
        if (!err) g.lastFile = Utf8ToWide(path);
      }
    }
  }

  g.camWin.Create(hInst, &g.camera);
  g.stopWin.Create(hInst, g.hwnd);
  g.camCtl.Create(hInst, &g.camWin);

  ShowWindow(g.hwnd, SW_SHOW);
  UpdateWindow(g.hwnd);

  bool running = true;
  while (running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running) break;

    // Acciones que no deben ocurrir en medio de un cuadro de ImGui.
    if (g.wantRefresh) RefreshDevices();
    if (g.finished) {
      g.finished = false;
      FinishFlow();
    }
    if (g.wantStop) {
      g.wantStop = false;
      if (g.mode == App::Mode::Recording) EndRecordingFlow();
    }
    if (g.wantStart) {
      g.wantStart = false;
      if (g.mode == App::Mode::Idle) BeginRecordingFlow();
    }
    if (g.wantBrowse) {
      g.wantBrowse = false;
      const std::wstring dir = PickFolder(g.hwnd);
      if (!dir.empty()) {
        g.st.outputDir = dir;
        CopyStr(g.outDirBuf, sizeof(g.outDirBuf), WideToUtf8(dir));
        g.dirty = true;
      }
    }

    SyncDevices();
    if (g.dirty) {
      g.dirty = false;
      g.st.Save();
    }

    if (!IsWindowVisible(g.hwnd) || IsIconic(g.hwnd)) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 60, QS_ALLINPUT);  // oculta: casi no consume CPU
      continue;
    }

    // Último fotograma de la cámara para la vista previa.
    int fw = 0, fh = 0;
    if (g.camera.GetFrame(g.camBuf, fw, fh, g.camSerial)) {
      // La vista previa es pequeña: reducimos por promedio de bloques (Full HD -> ~640 px) para que se vea nítida.
      const uint8_t* up = g.camBuf.data();
      int uw = fw, uh = fh;
      const int k = fw / 640;
      if (k >= 2) {
        ReduceBox(g.camBuf.data(), fw, fh, k, g.camSmall, uw, uh);
        up = g.camSmall.data();
      }
      if (!g.camTex.srv || g.camTex.w != uw || g.camTex.h != uh) {
        MakeTexture(g.camTex, up, uw, uh, DXGI_FORMAT_B8G8R8A8_UNORM);
      } else {
        g.ctx->UpdateSubresource(g.camTex.tex.Get(), 0, nullptr, up, (UINT)(uw * 4), 0);
      }
      g.camAspect = (float)fw / (float)fh;
      if (g.photoOverlay && !g.photoBusy) g.photoOverlay = false;  // la vista previa volvió
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();

    const float clear[4] = {14.f / 255.f, 12.f / 255.f, 59.f / 255.f, 1.f};
    ID3D11RenderTargetView* rt = g.rtv.Get();
    g.ctx->OMSetRenderTargets(1, &rt, nullptr);
    g.ctx->ClearRenderTargetView(g.rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = g.swap->Present(1, 0);
    g.occluded = (hr == DXGI_STATUS_OCCLUDED);
  }

  // Cierre ordenado: nunca perder una grabación en curso.
  g.camCtl.End();
  g.camWin.Hide();
  g.stopWin.Hide();
  if (g.stopThread.joinable()) g.stopThread.join();
  if (g.photoThread.joinable()) g.photoThread.join();
  if (g.camRecording) g.camRec.Stop();  // nunca perder un video de cámara en curso
  if (g.camStopThread.joinable()) g.camStopThread.join();
  if (g.rec.Recording()) g.rec.Stop();
  RestoreSystemCursors(CursorMarkerPath());
  g.st.Save();
  g.camera.Stop();
  g.desktop.Stop();
  g.mic.Stop();
  g.camCtl.Destroy();
  g.camWin.Destroy();
  g.stopWin.Destroy();

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g.camTex = Tex{};
  g.logo = Tex{};
  g.rtv.Reset();
  g.swap.Reset();
  g.ctx.Reset();
  g.dev.Reset();

  MFShutdown();
  CoUninitialize();
  CloseHandle(mutex);
  return 0;
}
