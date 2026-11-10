#pragma once
// Registro de diagnóstico: %APPDATA%\MiauScreenRecorder\log.txt (se reinicia en cada arranque).
#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <string>

inline std::wstring LogPath() {
  PWSTR appdata = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) dir = appdata;
  CoTaskMemFree(appdata);
  if (dir.empty()) return L"miau-log.txt";
  dir += L"\\MiauScreenRecorder";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\log.txt";
}

inline void LogLine(const char* fmt, ...) {
  static int lines = 0;
  if (lines++ > 400) return;  // sin crecer sin límite
  FILE* f = nullptr;
  _wfopen_s(&f, LogPath().c_str(), L"ab");
  if (!f) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(f, "%s\r\n", buf);
  fclose(f);
}

inline void LogReset() {
  FILE* f = nullptr;
  _wfopen_s(&f, LogPath().c_str(), L"wb");
  if (f) fclose(f);
  // Versión de Windows (RtlGetVersion no miente como GetVersionEx).
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  OSVERSIONINFOW vi{sizeof(vi)};
  if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&vi);
  LogLine("Miau Screen Recorder " MIAU_VERSION " - Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion,
          vi.dwBuildNumber);
}
