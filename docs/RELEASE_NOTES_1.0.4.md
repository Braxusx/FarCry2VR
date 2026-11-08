# Far Cry 2 VR 1.0.4

Mod v234, launcher 1.0.4. Updates 1.0.x in place: run the launcher, pick the game folder and press **Install**. Your settings are kept and new options are added. Report problems with the logs from the launcher (**Collect logs**).

## Smoother play
- **No more jittering body, arms and weapon while moving.** The view now follows the body update the game is drawing, not the newest one, which was one game step ahead. Existing settings files are updated automatically (`CameraOnBodyClock` 1 → 2).
- **About 1.5 ms less CPU work per frame.**
  - The game no longer waits for the graphics card at the end of each frame. The graphics card hands each finished eye image to the headset by itself, and each image is synchronised on its own (`EndFrameWait=0`).
  - The game's slow check of every graphics buffer it writes is replaced by a simple one (`FastMapCheck=1`).
  - This mainly helps PCs where the game runs close to the headset's frame time.
- **DLSS frames are handed over on the graphics card too** (`DlssGuideHandover=1`). At most one frame is in flight, so weapons held in your hands stay steady when the graphics card is near its limit (`FrameAheadLimit=1`).
- **Best CPU cores:** the game runs on the performance cores (Intel) or the 3D V-Cache cores (AMD X3D) (`CpuPlacement=1`). An optional higher game priority is also available (`CpuPriority`).
- **Body no longer turns while strafing:** the game's own gamepad aim assist is switched off (`GameAimAssist=0`).
- **Two-handed grip holds** through short controller tracking dropouts.
- **Desktop V-Sync no longer slows the headset** (`DesktopVsync=0`).
- **Sun shadows** snap to whole shadow-map texels, so they shimmer less when you move (`ShadowSnap=1`).

## Weapons
- **Guns point where your controller points.** The barrel follows the controller, and each gun is turned so its own sights line up with the aiming dot. This replaces the old learned holds and the End-key hand angle menus.
  - Holding the gun two-handed carries the dot with it.
  - The scope view starts past the weapon, so the crossbow no longer covers its own scope (`ScopeViewForward`).
- **Weapon alignment menu (F1):** fine-tune any gun in the headset.
  - Up/Down choose **Turn the gun** or **Move the dot**. Enter (or the right stick click) ticks a row, then the right stick adjusts it. Untick to save.
  - Each gun keeps its own setting (`[Sights]` in fc2vr.ini). Settings for several common guns are included.
  - The key can be changed with `WeaponAlignKey`.
- **Machete** held upright in a fixed, natural pose; it's now recognised from the weapon itself, so other weapons never take its angles.
- **Mounted guns aimed with the controllers:**
  - Hold either grip on a turret and point that controller to aim; your hands stay on the gun's handles. Released, the sticks aim as before.
  - Lowering the controller raises the barrel, like pushing down on the rear handles (`TurretMotionInvertY=0` reverses it).
  - On a vehicle's gun, the right grip aims instead of changing seat; A still changes seat.
  - Can be switched off in the launcher (`TurretMotionAim`).

## Interaction
- **Hand prompt where you look:** objects, weapons on the ground, vehicles and diamond cases are offered when they are near the centre of your view (`UseFromHead`, `UseCone`, `UseCenter`). Diamond cases open from any angle you can see them.
- **The hand icon, its glow and the weapon-swap icons** sit together in the centre of the view (`UiPromptCenter`).
- **No accidental weapon switch in menus:** holster grabs and B no longer change weapons while a menu is open.

## VR HUD
All of these can be switched off in the launcher's Optional page or in fc2vr.ini.
- **Ammo counter on the gun:** rounds in the clip and spare rounds, beside the weapon in your right hand (`AmmoPanel*`).
- **Grenade counter** by your left hand when you aim a throw (`GrenadePanel*`).
- **Clock under the map** (`MapClock24h=1` for 24 h), with your **health bar and syrettes** beside it (`MapClock*`, `MapHealth*`).

## Game content
- **The 7 Intel predecessor missions** unlocked: an anonymous phone call in the central town (`PredecessorMissions`).
- **Bonus machetes** (Primitive, Homemade): main menu > Options > Game > Machete Type (`BonusMachetes`).
- **Jackal tape fix:** tape #9 no longer repeats on the southern map, a patch 1.03 bug (`JackalTapeFix`). Thanks to FoxAhead for the analysis of this bug; the fix is the mod's own code. *Not tested yet: it needs Act 2.*

## Optional gameplay
These are off by default; turn them on in the launcher's Optional page.
- **No malaria attacks** (timed attacks only; story attacks still happen) and **unlimited malaria pills** (`MalariaAttacks`, `UnlimitedPills`). *Unlimited pills is not tested yet.*
- **Indestructible dune buggy, hang glider and ATV** (`IndestructibleBuggy`, `IndestructibleGlider`, `IndestructibleATV`).
- **Turn the body with your head** and **Steady grass when turning** (Steam version) (`HeadsetBodyYaw`, `GrassHeadYawFix`). Contributed by luisparravicini.

## Launcher
- **Game settings check:** the Overview page checks the game's video settings (DirectX 10, anti-aliasing None, V-Sync Off). The **Fix game settings** button corrects them and keeps a backup of GamerProfile.xml.
- New **VR HUD**, **Gameplay** and **Performance** options on the Optional page.

## Fixes
- **GOG:** the frame pacing hook now works on the GOG version (two missing address mappings). Contributed by thefreemike31.
- Shoulders no longer swap when you turn past 90° (luisparravicini).

## Known
- Puddles in a few raised spots reflect the wrong scenery, and some distant shadows flicker. Both are original game issues that also happen without the mod.
- Diagnostics: setting `DiagLog=1` in fc2vr.ini adds periodic troubleshooting lines to fc2vr.log.

**Requirements:** Windows 10/11 and Far Cry 2 (Steam or GOG), **set to DirectX 10** with **anti-aliasing set to None**. You also need an OpenXR runtime with 32-bit support (for example Virtual Desktop VDXR, or SteamVR 2.17+). DLSS needs an NVIDIA RTX GPU.

The launcher includes the NVIDIA DLSS runtime, redistributed as part of this mod; it isn't in the source tree. This product uses NVIDIA DLSS technology. This is an unofficial fan mod, not affiliated with Ubisoft.
