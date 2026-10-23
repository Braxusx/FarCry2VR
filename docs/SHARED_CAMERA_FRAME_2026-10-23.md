# Shared effective camera frame — offline implementation checkpoint

Date: 2026-10-23. Branch: `codex/tracking-arms-physical-steering`.
Parent: `a41b3b9` (height/direction fix); upstream base: `92a29c5` (1.0.1).
This is a development checkpoint, not a release or headset acceptance claim.

## Contract and implementation

Hypothesis: a single effective game origin/yaw, applied to the same tracking-space
poses, removes the raw-hand/steadied-eye frame disagreement. The control holds the
controller/eye offsets fixed; the variable is native camera bob/yaw. Pass only if
the old route shows disagreement and the shared route preserves the fixed offsets.

`common/pose_frame.h` contains the existing smoothing policy and pure transforms.
`PoseFrameBuilder` prepares at most one result per application sequence. The local
const frame snapshots raw/effective game bases, XR recenter origin/yaw/height,
world scale and a diagnostic sequence. The inverse mapping is also centralized.
No filtering was added to physical controller input.

The viewport adapter selects the route before preparing the frame. Posed stereo
or a valid head/weapon scope ray may use SteadyCamera under its existing normal-
control, non-vehicle, non-mounted gating. A native flat fallback, missing scope ray
or disabled stereo remains raw; its hands cannot be stabilized against an unposed
flat camera. A scope ray is sampled once for that decision and consumed consistently.

The prepared frame feeds both eye poses (preserving each eye's orientation/FOV),
culling pose, arm/weapon/head targets, hand interactions, flashlight, scope view,
AFW grip positions and reticle/arc coordinate conversions. Existing camera
save/restore and renderer re-entry are unchanged. World scale affects positions,
not camera direction vectors. HeightOffset retains the R5 separation.

`arms::Frame` publishes the effective game basis and **absolute** origin under the
existing critical section. Apply/interaction paths no longer add later Camera-bone
motion to a saved offset. Native camera rows/bone matrices still serve animation
mapping and bone-axis calibration; native turret aim and spread interpretation
remain raw. This does not transfer gameplay authority to render-only late updates.
No new native address/call/hook, configuration key, deployed file or package.

## Offline evidence

Tests are built from the production helpers, except the deliberately frozen
original SteadyCam oracle in `mod/tests/fixtures/steady_camera_v230.h`. Its function
body was compared to `a41b3b9:mod/src/main.cpp` and matched exactly after line-ending
normalization; only native-query stubs surround it. It is not in the DLL source list.

| New group | Checks | Result |
| --- | ---: | --- |
| bob | 1,209 | Fixed eye/hand offsets retained; old raw/steady origin gap ~0.0948063 at synthetic 0.10-unit bob, shared gap zero |
| yaw | 811 | Running yaw sway shares one basis; deliberate stick/snap turns stay immediate; old yaw split exercised |
| policy | 24,000 | 1,000 samples agree with original policy within 0.00002 per component: running/slow travel, yaw wrap, bob, crouch, jumps, mode exits/reentry and invalid dt |
| once | 156 | Repeated consumers cannot advance history or replace origin/scale/reference; bypass/reentry controls pass |
| inverse | 3,825 | Scale/height/yaw sweeps round-trip XR/reference/world positions/directions; canted eye angles preserved |
| bone | 11 | Published target independent of later animated Camera-bone translation; old reconstruction moves it |

These 30,012 checks plus the existing 736 R5 checks passed under MSVC Win32 Release
19.50.35729 and GCC i686 16.2.0, warnings as errors for test builds. A separate CMake
source-wiring guard verifies preparation/publication ordering, one raw-camera
sampling site, actual adapter usage and retained raw calibration. MSVC CTest passed
all 12 groups. These are synthetic/source results, not observed Dunia behavior.

The full x86 GCC DLL builds with the same 32 pre-existing diagnostics as its parent
(matching warning text after stripping source line numbers). PE32/i386, undecorated
`DirectInput8Create` and Windows system imports checked. Documented llvm-mingw and
launcher/DLSS-helper builds remain unverified; no compiler installation was changed.
Builds/tests are ignored under `build/shared-camera-1/` and `build/tracking-tests-msvc/`.
The startup banner is `contribution shared-camera-1` and includes the loaded path.

Verified development DLL: `build/shared-camera-1/dinput8.dll` (1,220,680 bytes).
SHA-256: `51daec95f9a3f3c6efdd73bef324d704e954925ff500210e9d9ef0db2371c0f4`.
Rebuilding changes the compile-time banner and therefore this hash.

## Remaining runtime gate — important

An immutable shared transform does **not** prove when native hierarchy jobs consume
it. Hooks at the existing hierarchy paths may run before viewport publication and
use an older sample. Removing the old bone-relative refresh could expose that lag.
No new native ordering or bone-cache ownership has been claimed from these tests.
There is deliberately no guessed render-thread skeleton rerun or second late pose
writer. Do not merge/ship this checkpoint as a proved movement fix without this gate.

`pose frame shared-camera-1` logs raw/effective bases every 900 samples. `pose use`
logs published versus last successfully applied/interacted sequence, validity,
call counts and thread IDs after viewport rendering every 90 samples. Zero/missing
usage without its valid flag and advancing call count is not evidence of alignment.
The sequence is observational only: session/space epochs, input freshness, cached
ray ownership and invalidation remain R4, not implemented here.

On an explicitly authorized runtime/HMD test:

1. Verify the DLL banner/hash and calibration success; capture advancing apply and
   interaction counts plus their sequences relative to the viewport. If hierarchy
   work consistently uses an older sample, stop acceptance and establish a safe
   presentation-clock/ownership seam before adding a rebase. Never call Apply from
   the renderer or restore animated origin following just to hide the difference.
2. Compare stationary controllers during walk/sprint, head-only motion, deliberate
   turns, crouch and recenter, with SteadyCamera on/off. Confirm no added hand lag,
   yaw drift, weapon-relative jumps, eye skew or stale-frame reuse.
3. Exercise scope/monocular transitions and missing-ray fallback, flashlight,
   aim/reticle/throw arc, reload/map/phone presentation, native flat fallback,
   vehicle entry/exit and mounted aiming. Check asymmetric eye views if available.
4. Restore user settings and close only the game process owned by that test.

R2 twist, R3 animated support socket, R4 stale authority, R6/R7 arm/rig robustness,
R8 spread and R9 image refusal remain open. No reload, steering or DQS work is
claimed. Roll back this shared-frame commit as a whole to `a41b3b9` if necessary;
the independently reviewable R5 fix remains. Nothing has been pushed or deployed.
