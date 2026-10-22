#!/usr/bin/env python3
"""Pack the launcher payload: launcher/payload/* -> launcher/payload_lzma/* (LZMA-alone, preset 9|extreme,
64 MiB dictionary, real uncompressed size in the header - the launcher's LzmaDec reads it).

launcher/payload/ must contain:
  dinput8.dll            - the mod (mod/build.sh -> mod/build/dinput8.dll)
  fc2vr_dlss_host64.exe  - the DLSS helper (mod/build.sh with NGX set)
  openxr_loader.dll      - Khronos OpenXR loader (x86, from the OpenXR SDK release)
  nvngx_dlss.dll         - NVIDIA DLSS runtime (from https://github.com/NVIDIA/DLSS, lib/Windows_x86_64/rel)
  fc2vr.ini              - default settings (in this repository)
"""
import lzma, os, struct, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'launcher')
src, dst = os.path.join(root, 'payload'), os.path.join(root, 'payload_lzma')
os.makedirs(dst, exist_ok=True)
for name in ['dinput8.dll', 'fc2vr_dlss_host64.exe', 'openxr_loader.dll', 'nvngx_dlss.dll', 'fc2vr.ini']:
    p = os.path.join(src, name)
    if not os.path.exists(p):
        sys.exit('missing ' + p)
    data = open(p, 'rb').read()
    flt = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME, 'dict_size': 1 << 26}]
    c = bytearray(lzma.compress(data, format=lzma.FORMAT_ALONE, filters=flt))
    c[5:13] = struct.pack('<Q', len(data))
    open(os.path.join(dst, name), 'wb').write(c)
    print('%-24s %10d -> %10d' % (name, len(data), len(c)))
