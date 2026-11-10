#pragma once

#include "common.h"

// Muestra una capa a pantalla completa (como la herramienta de recortes de Windows)
// para elegir el área a grabar.
//   - Arrastrar con el mouse: define el rectángulo.
//   - F: toda la pantalla donde está el cursor.
//   - Esc o clic derecho: cancelar.
// Devuelve true si el usuario eligió un área. `out` queda en coordenadas del
// escritorio virtual (píxeles físicos), con ancho y alto pares.
// `cursorScalePct` / `cursorRgb` (-1 = original) hacen que el cursor del selector se vea como el elegido en Options.
bool SelectRegion(HINSTANCE hInst, RECT& out, int cursorScalePct = 100, int cursorRgb = -1);
