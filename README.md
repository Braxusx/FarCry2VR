# Far Cry 2 VR

A native stereo VR mod for **Far Cry 2** (Dunia engine) with full motion controls, by **Braxus**.

The game renders each eye itself through its own engine, with the headset's per-eye projection, so the image has correct depth and scale. It is not a 3D re-projection. Output goes through **OpenXR**. Development and testing used a Meta Quest 3 through Virtual Desktop (VDXR), but any OpenXR runtime should work.

**Version 1.0.0**, mod build v229. Supports the **Steam** and **GOG** versions with the same DLL.

## Features

- **Native stereo rendering:**
  - every eye is a real engine render with the headset's off-axis projection;
  - one wide culling camera shared by both eyes;
  - sun shadows shared between the eyes, which brings busy towns to a solid 90 fps.
- **Head tracking,** with recentering (hold the left stick for 1 s, or F8).
- **Motion controls:** the Quest controllers drive the game's own gamepad input, so every menu, vehicle and button prompt works.
  - **Weapons in your hands:** each weapon has its own grip pose, two-handed holds, and aiming down sights and scopes by raising the gun to your eye.
  - **Body holsters:** rifle over the right shoulder, pistol at the right hip, machete in front of the chest.
  - **Physical machete swings.**
  - **Grenades and molotovs:** thrown with an arc preview.
  - **Optional VR climbing and NPC grabbing.**
  - **Map and GPS** held in the hand.
  - **Monocular:** with the map up, the right grip brings it to your eye, where the right trigger tags targets.
  - **Mounted guns,** aimed with either stick.
  - **Optional flashlight:** raise the left hand to your left temple and pull the trigger.
- **HUD and menus on floating panels;** the aiming dot follows the gun.
- **DLSS / DLAA** (NVIDIA RTX) through a small 64-bit helper process, with per-eye history.
- **Smooth or snap turning,** with an adjustable smooth-turn speed (Home key, then Left / Right arrows).
- **Optional graphics fixes:**
  - remove the brown colour filter;
  - remove rim lighting;
  - fix bouncing NPCs;
  - steady camera.
- **Launcher (ImGui):**
  - one-click install for the Steam or GOG game folder, with uninstall;
  - settings pages and a controls guide;
  - limits the game to 16 CPU cores, which the engine needs.

## Requirements

- Far Cry 2 (Steam or GOG) on Windows 10 or 11.
- An OpenXR runtime: Virtual Desktop (VDXR), SteamVR, Meta Quest Link and others.
- For DLSS, an NVIDIA RTX GPU. Without one, the mod runs without DLSS.

## Install

1. Download `FarCry2VR-1.0.0.zip` from the [Releases](../../releases) page and unzip it.
2. Run `FarCry2VR-Launcher_1.0.0.exe`, pick your Far Cry 2 folder (Steam or GOG), and press **Install**.
3. Start your OpenXR runtime (for example Virtual Desktop), then start the game from the launcher.

To install by hand instead, copy everything in the zip's `manual_install` folder into the game's `bin` folder: `dinput8.dll`, `fc2vr.ini`, `openxr_loader.dll`, `nvngx_dlss.dll` and `fc2vr_dlss_host64.exe`. Use the launcher to uninstall, or delete those files.

The NVIDIA DLSS runtime (`nvngx_dlss.dll`) is redistributed only inside the release zip, as part of the mod. It is never in this source tree.

## Controls (Quest controllers, on foot)

| Control | Action |
|---|---|
| Left stick | Move. Click: sprint. Hold in for 1 s: recenter. |
| Right stick | Turn. Pull down: crouch. Click: syrette (heal). |
| Right trigger | Fire. With the machete, swing it instead. |
| Left trigger | Reload / clear a jam. With the right grip held: throw. At your left temple: flashlight. |
| A | Jump. Left grip + A: re-take the weapon's hold. |
| B | Tap: next weapon. Hold: grenade / molotov. Left grip + B: weapon away / back. |
| X | Use: doors, turrets, pick-ups, vehicles. |
| Y | Map / GPS. Also answers the phone. |
| Left grip | Two-handed hold, grab, climb. |
| Right grip | At a holster spot: draw that weapon. Gun at your eye: sights / scope. With the map up: monocular. |
| Menu | Pause menu. |

**Vehicles:**

| Control | Action |
|---|---|
| Right trigger | Accelerate |
| Left trigger | Brake / reverse |
| Either stick | Steer |
| Left grip | Handbrake |
| Right grip or A | Change seat |
| X | Get out |
| B | Headlights |

**Keyboard:**

| Key | Action |
|---|---|
| F8 | Recenter |
| End | Hand-angle menu |
| Home | Smooth turning speed |
| F9 | Stereo on/off |

All settings live in `fc2vr.ini`, which has a comment on each key. The launcher edits them for you.

## Building from source

The build uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and runs on Linux or Windows (with MSYS2 or Git Bash).

```sh
# the mod (32-bit dinput8.dll proxy) and, with NGX set, the 64-bit DLSS helper
TC=/path/to/llvm-mingw/bin NGX=/path/to/ngx sh mod/build.sh

# the launcher: put the five payload files into launcher/payload, pack them, build
python3 tools/pack_payload.py
TC=/path/to/llvm-mingw/bin sh launcher/build.sh
```

`NGX` is a folder containing the NVIDIA DLSS SDK (`sdk/include`, `sdk/lib`) from https://github.com/NVIDIA/DLSS, plus an empty `dummy` folder. It is not included here. Without `NGX`, only the mod DLL is built.

**Source layout:**

| Path | Contents |
|---|---|
| `mod/src/main.cpp` | Engine hooks: stereo double render, eye cameras, culling, shadows, near pass, UI layers, settings |
| `mod/src/xr.cpp` | OpenXR session, swapchains and layers (projection, HUD/menu quads, scope, monocular ring) |
| `mod/src/controls.cpp` | Controllers → XInput gamepad, action-map tracking, gestures |
| `mod/src/arms.cpp` | Arms and weapons in the hands, holsters, machete, throwing, climbing, NPC grabbing, flashlight, monocular |
| `mod/src/dlss_*.cpp`, `mod/src/dlss_host/` | DLSS client (32-bit) and helper process (64-bit) |
| `mod/src/afw_fc2.cpp` | Alternate-frame warping (experimental, off by default) |
| `mod/src/gogmap.inc` | Steam → GOG address map (one DLL for both versions) |
| `launcher/` | ImGui launcher with an embedded, LZMA-compressed payload |

## Credits and license

Mod and launcher by **Braxus**, developed with Claude (Anthropic).

Licensed under the **GNU GPL v3** (see `LICENSE`). Third-party components keep their own licenses; see `THIRD_PARTY_NOTICES.md`.

This is an unofficial fan project, not affiliated with or endorsed by Ubisoft. Far Cry® is a trademark of Ubisoft Entertainment. No game files are included.

This product uses NVIDIA DLSS technology. NVIDIA and DLSS are trademarks of NVIDIA Corporation.
