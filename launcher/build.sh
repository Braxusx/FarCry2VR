#!/bin/sh
# Far Cry 2 VR launcher: llvm-mingw, 32-bit, static, payload embedded as RCDATA from ./payload
set -e
# TC = the bin folder of llvm-mingw; payload_lzma/ must be filled first (tools/pack_payload.py)
TC=${TC:?set TC to the llvm-mingw bin folder}
cd "$(dirname "$0")"
CXX=$TC/i686-w64-mingw32-clang++
RC=$TC/i686-w64-mingw32-windres
IM=third_party/imgui
mkdir -p build/obj
$RC -I src -I payload_lzma src/launcher.rc -O coff -o build/obj/launcher.res
TCC=$TC/i686-w64-mingw32-clang
[ build/obj/LzmaDec.o -nt src/lzma/LzmaDec.c ] || $TCC -O2 -c src/lzma/LzmaDec.c -o build/obj/LzmaDec.o
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  [ build/obj/$f.o -nt $IM/$f.cpp ] || $CXX -std=c++17 -O2 -I $IM -c $IM/$f.cpp -o build/obj/$f.o
done
for f in imgui_impl_win32 imgui_impl_dx11; do
  [ build/obj/$f.o -nt $IM/backends/$f.cpp ] || $CXX -std=c++17 -O2 -I $IM -c $IM/backends/$f.cpp -o build/obj/$f.o
done
$CXX -std=c++17 -O2 -Wall -Wno-unused-function -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX -I $IM -I $IM/backends -I src \
  src/app.cpp src/bindings.cpp src/game.cpp src/theme.cpp src/zip.cpp build/obj/LzmaDec.o build/obj/imgui*.o build/obj/launcher.res \
  -mwindows -municode -static -static-libgcc -static-libstdc++ \
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lshcore -lole32 -loleaut32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32 \
  -o build/FarCry2VR-Launcher.exe
ls -la build/FarCry2VR-Launcher.exe
