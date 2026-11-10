#!/usr/bin/env bash
# Recrea third_party/ffmpeg (FFmpeg 9.0 estático y mínimo, LGPL) desde el código fuente oficial.
# Requisitos (Linux): g++-mingw-w64-x86-64, make, nasm, pkg-config, git.
set -euo pipefail
WORK="${1:-$PWD/ffbuild}"; PREFIX="$WORK/prefix"
mkdir -p "$WORK"; cd "$WORK"
[ -d ffmpeg ] || git clone --depth 1 --branch n9.0 https://github.com/FFmpeg/FFmpeg.git ffmpeg
[ -d nvh ] || git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers.git nvh
make -C nvh install PREFIX="$PREFIX"
cd ffmpeg
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure --prefix="$PREFIX" \
  --enable-cross-compile --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- \
  --pkg-config=pkg-config --pkg-config-flags=--static \
  --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --disable-network --disable-avdevice --disable-avfilter --disable-swresample \
  --disable-everything --enable-small \
  --enable-ffnvcodec --enable-nvenc --enable-d3d11va --enable-mediafoundation \
  --enable-encoder=aac,h264_nvenc,h264_mf,mpeg4 \
  --enable-muxer=matroska,mp4 --enable-demuxer=matroska \
  --enable-parser=h264,aac,mpeg4video --enable-protocol=file \
  --extra-cflags="-O2"
make -j"$(nproc)" && make install
echo "Listo. Copia $PREFIX/include y $PREFIX/lib/*.a a third_party/ffmpeg/"
