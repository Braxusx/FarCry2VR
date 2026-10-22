# Far Cry 2 VR 1.0.1

Mod build v230, launcher 1.0.1.

**Fixes**
- **64 fps cap on some Windows 11 PCs:** Windows 11 drops a game's 1 ms timer to 15.6 ms while its window is in the background (as it usually is in VR), which capped the frame rate at about 64 fps. The mod now keeps the precise timer (`TimerFix=1`, on by default).

**Diagnostics**
- `fc2vr.log` gets a pacing line every 10 s: real fps, the headset refresh rate the runtime paces to, time spent waiting on the runtime, GPU time per frame, Windows timer resolution, the foreground app and whether the game window is minimized.
- The headset's refresh rate is logged (and can be requested with `DisplayRefreshRate=` in `fc2vr.ini`, 0 = leave it to the runtime).
- The launcher's **Collect logs** zip now includes an extended system report: CPU, power plan, GPU drivers, displays and refresh rates, NVIDIA driver profile settings (frame-rate limiter, V-Sync, low latency), Windows graphics settings (HAGS, Game Mode, Game DVR, per-app GPU, compatibility flags), OpenXR runtimes, Virtual Desktop version and running overlay / limiter / capture tools.

**Requirements:** Windows 10/11, Far Cry 2 (Steam or GOG) **set to DirectX 10** with **anti-aliasing set to None**, an OpenXR runtime (e.g. Virtual Desktop VDXR, SteamVR, Quest Link). DLSS needs an NVIDIA RTX GPU.

Install: run `FarCry2VR-Launcher_1.0.1.exe`, pick the game folder, press **Install** (updates 1.0.0 in place). Includes the NVIDIA DLSS runtime, redistributed as part of this mod (not in the source tree). This product uses NVIDIA DLSS technology. Unofficial fan mod, not affiliated with Ubisoft.
