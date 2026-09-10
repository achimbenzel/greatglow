# Cross-compiles the plug-in for Windows x64 from Linux with mingw-w64.
#
# This exists so the Windows build can be verified on a Linux CI machine.
# Visual Studio 2022 remains the supported toolchain for release builds.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets ctest run the Windows test binaries.
find_program(WINE_EXECUTABLE NAMES wine64 wine
             PATHS /usr/lib/wine /usr/lib64/wine /usr/local/bin /usr/bin
             NO_CMAKE_FIND_ROOT_PATH)
if(WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")
endif()
