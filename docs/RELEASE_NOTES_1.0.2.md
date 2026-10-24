# Far Cry 2 VR 1.0.2

Mod build v231, launcher 1.0.2.

**New**
- **Room-scale walking** (Settings > Controls > Movement, off by default): walking around your room moves you in the game. Your head can move 10 cm from your body (leaning) before the body follows; walls stop you about 30 cm short. Tuning in `fc2vr.ini`: `RoomScaleLeash`, `RoomScaleMaxSpeed`, `RoomScaleWallDistance`.
- **Head-based movement** (Settings > Controls > Movement > Walking direction): the left stick walks you where you look instead of where your body faces (`MovementDirection=1`).
- **OpenXR runtime check and choice:** Far Cry 2 is a 32-bit game and needs a runtime with 32-bit support. The launcher's Overview page now shows whether the runtime set up for 32-bit games is usable (with advice if not) and lets you pick an installed 32-bit-capable runtime for this game only. When OpenXR fails to start, `fc2vr.log` names the runtime the game was given and whether its DLL is 32- or 64-bit. Fixes "no picture in the headset" with 64-bit-only runtimes such as Pimax's PiOpenXR (use SteamVR 2.17+ or Virtual Desktop).

**Fixes and changes**
- **Tracking groundwork by phunkaeg** (GitHub PR #3): one shared camera frame for the eyes, hands, scope, flashlight and culling; `HeightOffset` no longer tilts the view; stricter handling of controller input when tracking drops out.
- **View and hands on the body's frame:** the headset view and the hands now follow the player's body on the same game frame (the game's camera was one frame behind the body). `CameraOnBodyClock`, `HandsOnBodyClock`.
- Extra logging for the monocular view (scene brightness while the monocular is at the eye) to track down a report of a black monocular view.

**Known issue:** the arms (not the hands or weapon) can still twitch slightly, more while moving. The bones the mod writes are smooth; the cause is in how the game skins the arm mesh, and is being investigated for 1.0.3.

**Requirements:** Windows 10/11, Far Cry 2 (Steam or GOG) **set to DirectX 10** with **anti-aliasing set to None**, an OpenXR runtime with 32-bit support (e.g. Virtual Desktop VDXR, SteamVR 2.17+). DLSS needs an NVIDIA RTX GPU.

Install: run `FarCry2VR-Launcher_1.0.2.exe`, pick the game folder, press **Install** (updates 1.0.x in place, your settings are kept). Includes the NVIDIA DLSS runtime, redistributed as part of this mod (not in the source tree). This product uses NVIDIA DLSS technology. Unofficial fan mod, not affiliated with Ubisoft.
