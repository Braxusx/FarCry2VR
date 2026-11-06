# Far Cry 2 VR 1.0.4 test 1

**Test build** (mod v234 test 52, launcher 1.0.4 test 1). Everything below has been tested in play except where noted. Please report problems with the logs from the launcher (**Collect logs** in the launcher).

**Smoother motion**
- **No more jittering body, arms and weapon while moving** ("stutter"): the view now follows the body update the game is drawing, not the newest one, which was one game step ahead. Existing settings files are updated automatically (`CameraOnBodyClock` 1 → 2).
- **Body no longer turns while strafing:** the game's own gamepad aim assist is switched off (`GameAimAssist=0`).
- **Two-handed grip holds** through short controller tracking dropouts.

**Interaction**
- **Hand prompt where you look:** objects, weapons on the ground, vehicles and diamond cases are offered when they are near the centre of your view, from your head rather than your body (`UseFromHead`, `UseCone`, `UseCenter`). Diamond cases open from any angle you can see them.
- **The hand icon, its glow and the weapon-swap icons** sit together in the centre of the view (`UiPromptCenter`).
- **No accidental weapon switch in menus:** holster grabs and B no longer change the mod's weapon slot while a menu is open (this twisted the gun after the pause menu).

**VR HUD (new, all can be switched off in the launcher's Optional page or fc2vr.ini)**
- **Ammo counter on the gun:** rounds in the clip and spare rounds beside the weapon in your right hand (`AmmoPanel*`).
- **Grenade counter** by your left hand for 3 seconds when you aim a throw with left grip + left trigger (`GrenadePanel*`).
- **Clock under the map** (12 h with AM/PM; `MapClock24h=1` for 24 h), and **health bar and syrettes** beside it (`MapClock*`, `MapHealth*`).

**Game content**
- **The 7 Intel predecessor missions** unlocked (anonymous phone call in the central town) (`PredecessorMissions`). *Not yet tested to the end.*
- **Bonus machetes** (Primitive, Homemade): main menu > Options > Game > Machete Type (`BonusMachetes`).
- **Jackal tape fix:** tape #9 "Stealing Boots" no longer repeats on the southern map (patch 1.03 bug) (`JackalTapeFix`). Thanks to FoxAhead for the analysis of this bug; the fix is the mod's own code. *Not yet tested in Act 2.*

**Optional gameplay (off by default, in the launcher's Optional page)**
- **No malaria attacks** (timed attacks; story attacks still happen) and **unlimited malaria pills** (`MalariaAttacks`, `UnlimitedPills`).
- **Indestructible dune buggy, hang glider and ATV** (`IndestructibleBuggy`, `IndestructibleGlider`, `IndestructibleATV`). The hang glider is not yet tested.

**Known**
- Diagnostics are still on in this test build (extra lines in fc2vr.log).
- Puddles in a few raised spots reflect the wrong scenery: an original game bug (also without the mod).

**Requirements:** Windows 10/11, Far Cry 2 (Steam or GOG) **set to DirectX 10** with **anti-aliasing set to None**, an OpenXR runtime with 32-bit support (e.g. Virtual Desktop VDXR, SteamVR 2.17+). DLSS needs an NVIDIA RTX GPU.

Install: run `FarCry2VR-Launcher_1.0.4_test1.exe`, pick the game folder, press **Install** (updates 1.0.x in place, your settings are kept and new options are added). Includes the NVIDIA DLSS runtime, redistributed as part of this mod (not in the source tree). This product uses NVIDIA DLSS technology. Unofficial fan mod, not affiliated with Ubisoft.
