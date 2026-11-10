#include "recorder.h"

#include "camera.h"

#include <shlobj.h>
#include <timeapi.h>  // timeBeginPeriod / timeEndPeriod (winmm)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

std::wstring Timestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  wchar_t buf[64];
  std::wcsftime(buf, 64, L"%Y-%m-%d_%H-%M-%S", &tm);
  return buf;
}

}  // namespace

bool Recorder::Start(const RECT& region, const RecorderSettings& s, AudioCapture* desktop, AudioCapture* mic,
                     std::string& err) {
  if (running_) {
    err = "Ya hay una grabación en curso.";
    return false;
  }

  cam_ = nullptr;
  prefix_ = L"Miau_";
  cap_.SetCursorStyle(s.cursorScalePct, s.cursorRgb);
  if (!cap_.Init(region, s.cursor, err)) return false;

  fps_ = std::clamp(s.fps, 10, 120);
  mp4_ = s.mp4;
  separate_ = s.separateTracks;
  desktop_ = desktop;
  mic_ = mic;
  hasDesktop_ = desktop && desktop->Running();
  hasMic_ = mic && mic->Running();
  audioTracks_ = (hasDesktop_ || hasMic_) ? ((separate_ && hasDesktop_ && hasMic_) ? 2 : 1) : 0;

  SHCreateDirectoryExW(nullptr, s.outputDir.c_str(), nullptr);
  const std::wstring stamp = Timestamp();
  finalPath_ = s.outputDir + L"\\" + prefix_ + stamp + (mp4_ ? L".mp4" : L".mkv");
  recPath_ = mp4_ ? (s.outputDir + L"\\" + prefix_ + stamp + L".recording.mkv") : finalPath_;

  static const double bitsPerPixel[3] = {0.07, 0.11, 0.18};
  const double bpp = bitsPerPixel[std::clamp(s.quality, 0, 2)];
  const double kbps = (double)cap_.Width() * cap_.Height() * fps_ * bpp / 1000.0;

  EncoderConfig ec;
  ec.path = recPath_;
  ec.width = cap_.Width();
  ec.height = cap_.Height();
  ec.fps = fps_;
  ec.videoBitrateKbps = (int)std::clamp(kbps, 1500.0, 60000.0);
  ec.audioStreams = audioTracks_;
  if (!enc_.Open(ec, err)) {
    cap_.Shutdown();
    return false;
  }

  timeBeginPeriod(1);  // temporizadores de 1 ms para un muestreo de video estable
  if (hasDesktop_) desktop_->BeginRecording();
  if (hasMic_) mic_->BeginRecording();

  stop_ = false;
  result_ = RecorderResult{};
  t0_ = std::chrono::steady_clock::now();
  running_ = true;
  videoTh_ = std::thread(&Recorder::VideoLoop, this);
  if (audioTracks_ > 0) audioTh_ = std::thread(&Recorder::AudioLoop, this);
  return true;
}

bool Recorder::StartCamera(Camera* cam, const RecorderSettings& s, AudioCapture* mic, std::string& err) {
  if (running_) {
    err = "A recording is already in progress.";
    return false;
  }
  if (!cam || !cam->Running()) {
    err = "The camera is not ready.";
    return false;
  }
  // Primer cuadro: define el tamaño del video.
  camSerial_ = 0;
  camBuf_.clear();
  int w = 0, h = 0;
  for (int i = 0; i < 60 && camBuf_.empty(); i++) {
    if (cam->GetFrame(camBuf_, w, h, camSerial_)) break;
    Sleep(50);
  }
  if (camBuf_.empty() || w < 16 || h < 16) {
    err = "The camera is not delivering images yet.";
    return false;
  }
  camSrcW_ = w;
  camSrcH_ = h;
  camW_ = w & ~1;  // los codificadores YUV 4:2:0 necesitan dimensiones pares
  camH_ = h & ~1;
  cam_ = cam;
  prefix_ = L"Miau_Camera_";

  fps_ = 30;
  mp4_ = s.mp4;
  separate_ = false;
  desktop_ = nullptr;
  mic_ = mic;
  hasDesktop_ = false;
  hasMic_ = mic && mic->Running();
  audioTracks_ = hasMic_ ? 1 : 0;

  SHCreateDirectoryExW(nullptr, s.outputDir.c_str(), nullptr);
  const std::wstring stamp = Timestamp();
  finalPath_ = s.outputDir + L"\\" + prefix_ + stamp + (mp4_ ? L".mp4" : L".mkv");
  recPath_ = mp4_ ? (s.outputDir + L"\\" + prefix_ + stamp + L".recording.mkv") : finalPath_;

  static const double bitsPerPixel[3] = {0.07, 0.11, 0.18};
  const double kbps = (double)camW_ * camH_ * fps_ * bitsPerPixel[std::clamp(s.quality, 0, 2)] / 1000.0;

  EncoderConfig ec;
  ec.path = recPath_;
  ec.width = camW_;
  ec.height = camH_;
  ec.fps = fps_;
  ec.videoBitrateKbps = (int)std::clamp(kbps, 1500.0, 60000.0);
  ec.audioStreams = audioTracks_;
  if (!enc_.Open(ec, err)) {
    cam_ = nullptr;
    return false;
  }

  timeBeginPeriod(1);
  if (hasMic_) mic_->BeginRecording();
  stop_ = false;
  result_ = RecorderResult{};
  t0_ = std::chrono::steady_clock::now();
  running_ = true;
  videoTh_ = std::thread(&Recorder::VideoLoop, this);
  if (audioTracks_ > 0) audioTh_ = std::thread(&Recorder::AudioLoop, this);
  return true;
}

// Último cuadro de la cámara (recortado a dimensiones pares si hace falta).
const uint8_t* Recorder::GrabCamera() {
  int w = 0, h = 0;
  if (cam_->GetFrame(camTmp_, w, h, camSerial_) && w == camSrcW_ && h == camSrcH_) camBuf_.swap(camTmp_);
  if (camBuf_.empty()) return nullptr;
  if (camW_ == camSrcW_ && camH_ == camSrcH_) return camBuf_.data();
  camOut_.resize((size_t)camW_ * camH_ * 4);
  for (int y = 0; y < camH_; y++)
    std::memcpy(camOut_.data() + (size_t)y * camW_ * 4, camBuf_.data() + (size_t)y * camSrcW_ * 4, (size_t)camW_ * 4);
  return camOut_.data();
}

void Recorder::VideoLoop() {
  using clock = std::chrono::steady_clock;
  int64_t lastPts = -1;

  while (!stop_) {
    const double elapsed = std::chrono::duration<double>(clock::now() - t0_).count();
    const int64_t pts = (int64_t)(elapsed * fps_);
    if (pts <= lastPts) {
      // Todavía no toca el siguiente fotograma.
      const auto due = t0_ + std::chrono::duration_cast<clock::duration>(
                                 std::chrono::duration<double>((double)(lastPts + 1) / fps_));
      std::this_thread::sleep_until(due);
      continue;
    }
    const uint8_t* px = cam_ ? GrabCamera() : cap_.Grab();
    if (px) enc_.WriteVideo(px, pts);
    lastPts = pts;
  }
}

void Recorder::AudioLoop() {
  using clock = std::chrono::steady_clock;
  const int rate = 48000;
  int64_t sent = 0;
  std::vector<float> bufD, bufM, mix;

  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const double elapsed = std::chrono::duration<double>(clock::now() - t0_).count();
    const int64_t expected = (int64_t)(elapsed * rate);
    int64_t need = expected - sent;
    if (need < rate / 100) continue;  // al menos 10 ms
    need = std::min<int64_t>(need, rate / 2);
    const size_t frames = (size_t)need;

    bufD.assign(frames * 2, 0.f);
    bufM.assign(frames * 2, 0.f);
    if (hasDesktop_) desktop_->Read(bufD.data(), frames);  // lo que falte queda en silencio
    if (hasMic_) mic_->Read(bufM.data(), frames);

    if (audioTracks_ == 2) {
      enc_.WriteAudio(0, bufD.data(), (int)frames);
      enc_.WriteAudio(1, bufM.data(), (int)frames);
    } else {
      mix.resize(frames * 2);
      for (size_t i = 0; i < frames * 2; i++) mix[i] = std::clamp(bufD[i] + bufM[i], -1.f, 1.f);
      enc_.WriteAudio(0, mix.data(), (int)frames);
    }
    sent += (int64_t)frames;
  }
}

void Recorder::Stop() {
  if (!running_) return;

  stop_ = true;
  if (videoTh_.joinable()) videoTh_.join();
  if (audioTh_.joinable()) audioTh_.join();

  if (hasDesktop_) desktop_->EndRecording();
  if (hasMic_) mic_->EndRecording();
  enc_.Close();
  cap_.Shutdown();
  cam_ = nullptr;
  timeEndPeriod(1);

  result_ = RecorderResult{};
  result_.path = finalPath_;
  result_.ok = true;

  if (mp4_) {
    std::string err;
    if (RemuxToMp4(recPath_, finalPath_, err)) {
      DeleteFileW(recPath_.c_str());
    } else {
      // Conserva el .mkv: la grabación no se pierde.
      result_.path = recPath_;
      result_.message = "No se pudo convertir a MP4 (" + err + "). Se conservó el archivo .mkv.";
    }
  }

  running_ = false;
}
