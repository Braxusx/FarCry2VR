# Far Cry 2 VR 1.0.4 test 5

**Test build** (mod v234 test 64, launcher 1.0.4 test 5). Everything below has been tested in play except where noted. Please report problems with the logs from the launcher (**Collect logs** in the launcher).

**New in test 5**
- **Fixed: the right eye twitching when turning the head** (test 3 and 4, mostly on GPUs running close to their limit). The faster frame hand-over from test 3 now synchronises each image passed to the headset on its own, so an eye image is never copied before the graphics card has finished it. With DLSS or AFW on, the game waits for the graphics card at the end of each frame as in test 2.

**New in test 4**
- **About 0.5 ms less work per frame:** the game checked every graphics buffer it writes with a slow Windows function that reads the memory back from the graphics card; the mod replaces it with a simple check (`FastMapCheck=1`; `0` restores the game's check). Together with test 3's change the game's render thread now does about 1.5 ms less work per frame.

**New in test 3**
- **About 1 ms more time per frame for the game:** the game no longer waits for the graphics card at the end of each frame before starting the next one; the graphics card hands the finished frame to the headset by itself. This mainly helps PCs where the game runs close to the headset's frame time (stutter or jerky movement at 72/80/90 Hz with frames close to the limit). The game's graphics now start through DXGI 1.1 (needed for this) (`EndFrameWait=0`; `EndFrameWait=1` restores the old behaviour).

**Smoother motion**
- **No more jittering body, arms and weapon while moving** ("stutter"): the view now follows the body update the game is drawing, not the newest one, which was one game step ahead. Existing settings files are updated automatically (`CameraOnBodyClock` 1 → 2).
- **Body no longer turns while strafing:** the game's own gamepad aim assist is switched off (`GameAimAssist=0`).
- **Two-handed grip holds** through short controller tracking dropouts.
- **Desktop V-Sync no longer slows the headset:** the desktop window never waits for the monitor, whatever the game's or the graphics driver's V-Sync setting (`DesktopVsync=0`).

**Launcher**
- **Game settings check:** the Overview page checks the game's own video settings (DirectX 10, anti-aliasing None, V-Sync Off) and a **Fix game settings** button corrects them (a backup of GamerProfile.xml is kept).
- New **VR HUD** and **Gameplay** sections on the Optional page for the options below.

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

Install: run `FarCry2VR-Launcher_1.0.4_test5.exe`, pick the game folder, press **Install** (updates 1.0.x in place, your settings are kept and new options are added). Includes the NVIDIA DLSS runtime, redistributed as part of this mod (not in the source tree). This product uses NVIDIA DLSS technology. Unofficial fan mod, not affiliated with Ubisoft.
