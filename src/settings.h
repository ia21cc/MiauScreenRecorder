#pragma once

#include "common.h"

#include <string>

// Ajustes del usuario. Se guardan en %APPDATA%\MiauScreenRecorder\settings.ini
struct Settings {
  std::wstring desktopId = L"default";  // L"" = no grabar audio del escritorio
  std::wstring micId = L"default";      // L"" = no grabar micrófono
  std::wstring cameraLink;              // L"" = elegir una automáticamente, L"none" = sin cámara

  int camSizePct = 22;   // ancho inicial de la cámara como % del ancho del área grabada (8..60); se recuerda el último tamaño usado

  std::wstring outputDir;  // carpeta de destino
  bool mp4 = true;
  int fps = 30;
  int quality = 1;  // 0 baja, 1 media, 2 alta
  bool cursor = true;
  bool photoMax = false;  // false: foto instantánea con la resolución de la vista previa; true: máxima de la cámara (tarda unos segundos)
  int cursorScalePct = 100;  // tamaño del cursor en el VIDEO (100..400 %); no cambia el cursor de Windows
  int cursorColor = 0;       // índice de color del cursor en el video (0 = original)
  bool separateTracks = false;

  void Load();
  void Save() const;
};
