cmake_minimum_required(VERSION 3.21)
project(MiauScreenRecorder VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "Este proyecto se compila con MinGW-w64 (MSYS2 en Windows, o cruzado desde Linux). "
                      "Ver README.md.")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Dear ImGui (se descarga solo) ──
include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE)

# ── FFmpeg 9.0 estático (LGPL), incluido en third_party/ffmpeg ──
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")

add_executable(MiauScreenRecorder WIN32
  src/main.cpp
  src/settings.cpp
  src/audio.cpp
  src/camera.cpp
  src/photo.cpp
  src/screen_capture.cpp
  src/region_selector.cpp
  src/overlays.cpp
  src/encoder.cpp
  src/recorder.cpp
  resources/app.rc
)

target_include_directories(MiauScreenRecorder PRIVATE src ${FFMPEG_DIR}/include)
target_link_directories(MiauScreenRecorder PRIVATE ${FFMPEG_DIR}/lib)

target_compile_definitions(MiauScreenRecorder PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_compile_options(MiauScreenRecorder PRIVATE -O2 -Wall -Wno-unused-parameter)

# Un solo .exe: runtime de C++ y FFmpeg enlazados estáticamente (solo usa DLL propias de Windows).
target_link_options(MiauScreenRecorder PRIVATE -municode -mwindows -static -s)

target_link_libraries(MiauScreenRecorder PRIVATE
  imgui
  avformat avcodec swscale avutil
  bcrypt mfuuid mfplat strmiids atomic
  d3d11 dxgi d3dcompiler dwmapi
  mf mfreadwrite
  ole32 oleaut32 uuid windowscodecs
  user32 gdi32 shell32 winmm imm32
)

set_target_properties(MiauScreenRecorder PROPERTIES OUTPUT_NAME "Miau Screen Recorder")
