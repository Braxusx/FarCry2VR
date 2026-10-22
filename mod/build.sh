#!/bin/sh
set -e
# TC = the bin folder of llvm-mingw (https://github.com/mstorsjo/llvm-mingw), e.g. TC=$HOME/llvm-mingw/bin sh build.sh
TC=${TC:?set TC to the llvm-mingw bin folder}
cd "$(dirname "$0")"
CXX=$TC/i686-w64-mingw32-clang++
mkdir -p build
$CXX -std=c++17 -O2 -Wall -Wno-unused-function -Wno-missing-braces \
  -I third_party/openxr_include \
  -shared -o build/dinput8.dll src/main.cpp src/xr.cpp src/perf.cpp src/hang.cpp src/controls.cpp src/arms.cpp src/dlss_fc2.cpp src/dlss_client.cpp src/afw_fc2.cpp src/gameaddr.cpp src/diag.cpp \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--kill-at -Wl,--enable-stdcall-fixup \
  -ld3d11 -ldxgi -lwinmm -luuid -lgdi32
$TC/llvm-objdump -p build/dinput8.dll | grep -E "DLL Name|DirectInput8Create" 
ls -la build/dinput8.dll
# the 64-bit DLSS helper: NGX SDK from https://github.com/NVIDIA/DLSS (NGX=<folder with sdk/include and sdk/lib>, plus
# NGX/dummy = an empty folder for the MSVC import stubs); MSVC CRT shims for the static NGX lib are in src/dlss_host/msvc_shim.cpp
if [ -z "$NGX" ]; then echo "NGX not set - DLSS helper skipped"; exit 0; fi
TC64=$TC/x86_64-w64-mingw32-clang++
$TC64 -std=c++17 -O2 -municode -mwindows -Wall -Wno-unused-function -Wno-unused-but-set-global -I $NGX/sdk/include \
  src/dlss_host/dlss_host.cpp src/dlss_host/msvc_shim.cpp $NGX/sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib -L$NGX/dummy \
  -o build/fc2vr_dlss_host64.exe -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
ls -la build/fc2vr_dlss_host64.exe
