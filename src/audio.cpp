#include "audio.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

std::vector<AudioDevice> ListAudioDevices(bool output) {
  std::vector<AudioDevice> out;

  ComPtr<IMMDeviceEnumerator> en;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
    return out;

  ComPtr<IMMDeviceCollection> col;
  if (FAILED(en->EnumAudioEndpoints(output ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col)))
    return out;

  UINT n = 0;
  col->GetCount(&n);
  for (UINT i = 0; i < n; i++) {
    ComPtr<IMMDevice> dev;
    if (FAILED(col->Item(i, &dev))) continue;

    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) continue;
    AudioDevice ad;
    ad.id = id;
    CoTaskMemFree(id);

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        ad.name = WideToUtf8(v.pwszVal);
      PropVariantClear(&v);
    }
    if (ad.name.empty()) ad.name = "Audio device";
    out.push_back(std::move(ad));
  }
  return out;
}

void AudioCapture::Start(const std::wstring& deviceId, bool loopback) {
  Stop();
  stop_ = false;
  running_ = false;
  th_ = std::thread(&AudioCapture::Run, this, deviceId, loopback);
}

void AudioCapture::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  running_ = false;
  peakL_ = 0.f;
  peakR_ = 0.f;
}

void AudioCapture::BeginRecording() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    head_ = 0;
    count_ = 0;
  }
  recording_ = true;
}

void AudioCapture::EndRecording() {
  recording_ = false;
  std::lock_guard<std::mutex> lk(mu_);
  head_ = 0;
  count_ = 0;
}

void AudioCapture::Push(const float* f, unsigned frames) {
  std::lock_guard<std::mutex> lk(mu_);
  const size_t cap = ring_.size();
  for (unsigned i = 0; i < frames; i++) {
    if (count_ + 2 > cap) {  // descarta siempre un frame completo (L+R)
      head_ = (head_ + 2) % cap;
      count_ -= 2;
    }
    const float l = f ? f[2 * i] : 0.f;
    const float r = f ? f[2 * i + 1] : 0.f;
    ring_[(head_ + count_) % cap] = l;
    ring_[(head_ + count_ + 1) % cap] = r;
    count_ += 2;
  }
}

size_t AudioCapture::Read(float* dst, size_t frames) {
  std::lock_guard<std::mutex> lk(mu_);
  const size_t cap = ring_.size();
  size_t avail = count_ / 2;

  // Si el reloj del dispositivo se adelanta, evita acumular latencia.
  if (avail > frames + 9600) {
    const size_t drop = avail - frames - 4800;
    head_ = (head_ + drop * 2) % cap;
    count_ -= drop * 2;
    avail = count_ / 2;
  }

  const size_t n = std::min(frames, avail);
  for (size_t i = 0; i < n * 2; i++) dst[i] = ring_[(head_ + i) % cap];
  head_ = (head_ + n * 2) % cap;
  count_ -= n * 2;
  return n;
}

void AudioCapture::Run(std::wstring id, bool loopback) {
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  [&] {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
      return;

    ComPtr<IMMDevice> dev;
    HRESULT hr;
    if (id == L"default")
      hr = en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &dev);
    else
      hr = en->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) return;

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) return;

    // Pedimos float32 estéreo 48 kHz; WASAPI convierte desde el formato del dispositivo.
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &wf, nullptr)))
      return;

    ComPtr<IAudioCaptureClient> cap;
    if (FAILED(client->GetService(IID_PPV_ARGS(&cap)))) return;
    if (FAILED(client->Start())) return;

    running_ = true;
    std::vector<float> silence;

    while (!stop_) {
      Sleep(10);

      float pl = 0.f, pr = 0.f;
      bool gotData = false;
      UINT32 pkt = 0;
      while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD fl = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
        gotData = true;

        const float* f = (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const float*>(data);
        if (f) {
          for (UINT32 i = 0; i < frames; i++) {
            pl = std::max(pl, std::fabs(f[2 * i]));
            pr = std::max(pr, std::fabs(f[2 * i + 1]));
          }
        }
        if (recording_) Push(f, frames);
        cap->ReleaseBuffer(frames);
      }

      if (gotData) {
        peakL_ = pl;
        peakR_ = pr;
      } else {
        // El loopback no entrega paquetes cuando no suena nada: deja caer el medidor.
        peakL_ = peakL_ * 0.8f;
        peakR_ = peakR_ * 0.8f;
      }
    }

    client->Stop();
  }();

  running_ = false;
  if (SUCCEEDED(hrCo)) CoUninitialize();
}
