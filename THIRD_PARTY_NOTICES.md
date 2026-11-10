# Avisos de terceros / Third-party notices

Miau Screen Recorder se distribuye bajo licencia MIT (ver `LICENSE`).
Usa los siguientes componentes de terceros:

## FFmpeg 9.0 (libavcodec, libavformat, libavutil, libswscale)

- Licencia: **LGPL v2.1 o posterior** (build propia sin `--enable-gpl` y sin `--enable-nonfree`).
  Texto en `third_party/ffmpeg/COPYING.LGPLv2.1` o en https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
- Sitio y código fuente: https://ffmpeg.org · https://github.com/FFmpeg/FFmpeg (etiqueta `n9.0`)
- Está **enlazado estáticamente** dentro del `.exe`. Para cumplir la LGPL, el código fuente completo de esta
  aplicación es público y se puede recompilar con otra build de FFmpeg (ver `third_party/ffmpeg/README.md` y
  `scripts/build-ffmpeg-static.sh`).
- Cabeceras de NVENC: nv-codec-headers (MIT), solo para compilar; la app carga el controlador de NVIDIA en tiempo de ejecución.

> Importante: si compilas FFmpeg con `--enable-gpl` (por ejemplo para x264/x265) o con
> `--enable-nonfree` (por ejemplo fdk-aac), **no** cumples con esta configuración y
> no podrías redistribuir el resultado bajo MIT.

## Dear ImGui

- Licencia: **MIT**
- Copyright (c) 2014-2025 Omar Cornut
- Sitio: https://github.com/ocornut/imgui

## Componentes opcionales de hardware (cargados en tiempo de ejecución)

- NVIDIA NVENC (cabeceras `nv-codec-headers`, licencia MIT)
- AMD AMF (cabeceras del SDK AMF, licencia MIT)
- Intel Quick Sync / oneVPL (licencia MIT)

Los codificadores de hardware se cargan desde los controladores del fabricante que
ya están instalados en el sistema del usuario.

## Notas sobre patentes de códecs

Las licencias de software libre no conceden licencias de patentes. H.264 y AAC pueden
estar cubiertos por patentes en algunas jurisdicciones. Este proyecto usa los
codificadores H.264 que vienen con el sistema (Media Foundation) o con la GPU del
usuario, y el codificador AAC nativo de FFmpeg. Consulta con un profesional legal si
planeas distribuir comercialmente.
