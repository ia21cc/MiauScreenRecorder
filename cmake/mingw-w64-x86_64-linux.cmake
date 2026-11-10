# Compilación cruzada desde Linux (Ubuntu/Debian: sudo apt install g++-mingw-w64-x86-64)
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64-linux.cmake
#   cmake --build build -j
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# Hace falta el modelo de hilos "posix" (std::thread).
find_program(MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ REQUIRED)
find_program(MINGW_CC  NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc REQUIRED)
find_program(MINGW_RC  NAMES x86_64-w64-mingw32-windres REQUIRED)
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_C_COMPILER ${MINGW_CC})
set(CMAKE_RC_COMPILER ${MINGW_RC})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
