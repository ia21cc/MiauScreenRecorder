# FFmpeg incluido (estático)

- **Versión:** FFmpeg 9.0 «Lei» (rama `n9.0`, libavcodec 63.1.102), compilada desde el código oficial
  (https://github.com/FFmpeg/FFmpeg) con MinGW-w64, **sin** `--enable-gpl` y **sin** `--enable-nonfree`.
- **Licencia:** LGPL 2.1 o posterior (`COPYING.LGPLv2.1`).
- **Contenido:** solo lo que usa la app: codificadores `aac`, `h264_nvenc` (NVIDIA), `h264_mf` (Media Foundation:
  AMD/Intel/NVIDIA/CPU) y `mpeg4` (respaldo); muxers `matroska` y `mp4`; demuxer `matroska`.
- **Reproducir la build:** `scripts/build-ffmpeg-static.sh` (la receta exacta de configure está ahí).

## Sobre la licencia LGPL y el enlace estático

FFmpeg queda enlazado dentro del `.exe`. La LGPL lo permite siempre que cualquiera pueda volver a enlazar la
aplicación con otra versión de FFmpeg: por eso el código fuente completo de Miau Screen Recorder (MIT) es
público y `scripts/build-ffmpeg-static.sh` + `CMakeLists.txt` permiten recompilarla con tu propia build de FFmpeg.
Al redistribuir el `.exe`, conserva `THIRD_PARTY_NOTICES.md` y enlaza al repositorio con el código fuente.
