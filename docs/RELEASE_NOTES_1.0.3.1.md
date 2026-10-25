# Far Cry 2 VR 1.0.3.1

Mod build v233, launcher 1.0.3.1. Same features and fixes as 1.0.3 (below).

**Change:** the launcher, the mod DLL and the DLSS helper are built without debug symbols. The 1.0.3 launcher carried ~430 KB of debug data after the program ("overlay"), which some antivirus heuristics treat as suspicious (VirusTotal 4/69 for 1.0.3: Bkav, McAfee reputation, SecureAge, Trapmine; Microsoft Defender, Kaspersky, Bitdefender, ESET, Avast, Malwarebytes and the other major engines clean). If your antivirus still warns, it is a false positive on a new, unsigned file; `manual_install/` in the zip installs without the launcher.


Mod build v233, launcher 1.0.3.

**Smoother motion**
- **Frame pacing:** the game measured each frame on the wall clock, so the player and every vehicle moved in slightly uneven steps and the world juddered past the view while moving. Every frame now moves the game on by exactly one headset frame; the difference is caught up gradually so game time stays on real time (`FramePacing`).
- **Steady arms and hands:**
  - the arm maths now run near the player instead of in world coordinates, where float rounding made the arms and hands shiver by 1-2 mm per frame (`ArmLocalFrame`);
  - the shoulders no longer swing ~12 cm with the running animation and pop where it loops (`ClavicleSmoothing`); the support hand no longer slides on a rifle while running (`ForegripSmoothing`);
  - a hand stabiliser (One Euro filter) removes hand tremor and tracking noise while still, and opens up for fast movement (`HandSmoothing`, `HandSmoothCutoff`, `HandSmoothBeta`, `HandSmoothRotBeta`).
- **Vehicles, taxi and bus rides:** the view now rides on your seat (the driver / passenger animation shook it against the car), placed from the same game update that is being drawn, and the hands follow it; Steady camera switches off automatically while you ride (`VehicleViewSmoothing`, `VehicleViewPrevious`).

**Fixes**
- **Black monocular view** with the game's HDR setting on: the scene is converted for the monocular window.
- **Tutorial messages and loading screens split or displaced:** the lowered top strip of the HUD is now cut at an empty row, or not at all when something crosses it.

**Tips**
- Below the headset's refresh rate (e.g. 60-68 fps at 90 Hz) the picture judders, most of all in vehicles. Lower the headset refresh rate (72 / 80 Hz), the render resolution or the DLSS quality mode. See the README.
- All new options are in `fc2vr.ini` with comments and on by default. With `DevKeys=1`, A/B test keys are available (listed in `fc2vr.ini`).

**Requirements:** Windows 10/11, Far Cry 2 (Steam or GOG) **set to DirectX 10** with **anti-aliasing set to None**, an OpenXR runtime with 32-bit support (e.g. Virtual Desktop VDXR, SteamVR 2.17+). DLSS needs an NVIDIA RTX GPU.

Install: run `FarCry2VR-Launcher_1.0.3.1.exe`, pick the game folder, press **Install** (updates 1.0.x in place, your settings are kept and new options are added). Includes the NVIDIA DLSS runtime, redistributed as part of this mod (not in the source tree). This product uses NVIDIA DLSS technology. Unofficial fan mod, not affiliated with Ubisoft.
