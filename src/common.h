#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#define MIAU_VERSION "1.0.0"

// Mensajes internos de la aplicación.
#define WM_APP_STOP (WM_APP + 1)      // pedir detener la grabación
#define WM_APP_FINISHED (WM_APP + 2)  // la grabación terminó de guardarse
#define WM_APP_PHOTO (WM_APP + 3)     // terminó de tomarse una foto de la cámara
#define WM_APP_CAMDONE (WM_APP + 4)   // terminó de guardarse un video de la cámara

inline std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

inline std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
