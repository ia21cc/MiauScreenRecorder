#include "settings.h"

#include <shlobj.h>

#include <algorithm>

namespace {

std::wstring IniPath(bool createDir) {
  PWSTR appdata = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) dir = appdata;
  CoTaskMemFree(appdata);
  if (dir.empty()) return L"settings.ini";
  dir += L"\\MiauScreenRecorder";
  if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\settings.ini";
}

std::wstring DefaultOutputDir() {
  PWSTR videos = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) dir = videos;
  CoTaskMemFree(videos);
  if (dir.empty()) dir = L".";
  return dir + L"\\Miau Screen Recorder";
}

std::wstring GetStr(const std::wstring& ini, const wchar_t* key, const std::wstring& def) {
  wchar_t buf[1024];
  GetPrivateProfileStringW(L"miau", key, L"\x1", buf, 1024, ini.c_str());
  return (buf[0] == L'\x1' && buf[1] == 0) ? def : std::wstring(buf);
}

int GetInt(const std::wstring& ini, const wchar_t* key, int def) {
  return (int)GetPrivateProfileIntW(L"miau", key, def, ini.c_str());
}

void PutStr(const std::wstring& ini, const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(L"miau", key, v.c_str(), ini.c_str());
}

void PutInt(const std::wstring& ini, const wchar_t* key, int v) { PutStr(ini, key, std::to_wstring(v)); }

}  // namespace

void Settings::Load() {
  const std::wstring ini = IniPath(false);
  desktopId = GetStr(ini, L"desktop", L"default");
  micId = GetStr(ini, L"mic", L"default");
  cameraLink = GetStr(ini, L"camera", L"");
  camSizePct = std::clamp(GetInt(ini, L"camSizePct", 22), 8, 60);
  outputDir = GetStr(ini, L"outputDir", DefaultOutputDir());
  mp4 = GetInt(ini, L"mp4", 1) != 0;
  fps = GetInt(ini, L"fps", 30);
  quality = std::clamp(GetInt(ini, L"quality", 1), 0, 2);
  cursor = GetInt(ini, L"cursor", 1) != 0;
  photoMax = GetInt(ini, L"photoMax", 0) != 0;
  cursorScalePct = std::clamp(GetInt(ini, L"cursorScale", 100), 100, 400);
  cursorColor = std::clamp(GetInt(ini, L"cursorColor", 0), 0, 15);
  separateTracks = GetInt(ini, L"separateTracks", 0) != 0;
}

void Settings::Save() const {
  const std::wstring ini = IniPath(true);
  PutStr(ini, L"desktop", desktopId.empty() ? L"" : desktopId);
  PutStr(ini, L"mic", micId);
  PutStr(ini, L"camera", cameraLink);
  PutInt(ini, L"camSizePct", camSizePct);
  PutStr(ini, L"outputDir", outputDir);
  PutInt(ini, L"mp4", mp4);
  PutInt(ini, L"fps", fps);
  PutInt(ini, L"quality", quality);
  PutInt(ini, L"cursor", cursor);
  PutInt(ini, L"photoMax", photoMax);
  PutInt(ini, L"cursorScale", cursorScalePct);
  PutInt(ini, L"cursorColor", cursorColor);
  PutInt(ini, L"separateTracks", separateTracks);
}
