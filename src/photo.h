#pragma once
#include "common.h"

#include <cstdint>
#include <string>

// Guarda una imagen BGRA (de arriba hacia abajo, alfa ignorado) como JPEG con el codificador de Windows (WIC).
// Debe llamarse desde un hilo con COM inicializado. `quality` va de 1 a 100.
bool SaveJpeg(const std::wstring& path, const uint8_t* bgra, int w, int h, int quality, std::string& err);
