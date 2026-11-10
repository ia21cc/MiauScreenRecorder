#include "photo.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

bool SaveJpeg(const std::wstring& path, const uint8_t* bgra, int w, int h, int quality, std::string& err) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    err = "No se pudo iniciar el codificador de imagen de Windows.";
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    err = "No se pudo crear el archivo de la foto.";
    return false;
  }
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
      FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    err = "No se pudo preparar el archivo JPEG.";
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&frame, &props))) {
    err = "No se pudo crear la imagen.";
    return false;
  }
  PROPBAG2 opt{};
  wchar_t name[] = L"ImageQuality";
  opt.pstrName = name;
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_R4;
  v.fltVal = (float)quality / 100.f;
  props->Write(1, &opt, &v);

  GUID fmt = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize((UINT)w, (UINT)h)) ||
      FAILED(frame->SetPixelFormat(&fmt))) {
    err = "No se pudo configurar la imagen.";
    return false;
  }

  const size_t rowBytes = (size_t)w * 3;
  std::vector<uint8_t> rgb(rowBytes * (size_t)h);
  for (int y = 0; y < h; y++) {
    const uint8_t* s = bgra + (size_t)y * w * 4;
    uint8_t* d = rgb.data() + (size_t)y * rowBytes;
    for (int x = 0; x < w; x++, s += 4, d += 3) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  }
  if (FAILED(frame->WritePixels((UINT)h, (UINT)rowBytes, (UINT)rgb.size(), rgb.data())) || FAILED(frame->Commit()) ||
      FAILED(enc->Commit())) {
    err = "No se pudo escribir la foto.";
    return false;
  }
  return true;
}
