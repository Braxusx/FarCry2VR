# Headset-driven body yaw

In `[VR]` in `fc2vr.ini`, enable **both** `HeadsetBodyYaw=1` and
`GrassHeadYawFix=1`. Both default to 0. On foot, physical horizontal head yaw
turns the native character and pawn headings. The same transfer is removed
from the eye, culling and controller basis, so the view turns once. Head-based
movement is adjusted into the new body frame; thumbstick yaw remains additive.
Pitch and roll do not turn the body. The feature works independently of
`WeaponInHand` and `AimFromController`.

The native adapter currently supports only Steam Dunia.dll `0,1,0,1`, SHA-256
`6353936A54AA841350BB30FF005727859CDEF1AA10C209209B220B399E862765`.
An image hash mismatch disables the adapter. It uses upstream's character
orientation interface (+0x88/+0x8C) and pawn headings
(+0x180/+0x18C/+0x310), on that exact image. Writes occur in the camera-component
update hook, require a fresh head source (100 ms), matching owner/reference,
normal on-foot controls and an upright character, and verify orientation
readback. A refused write disables further requests until reset. Vehicles,
mounted weapons, rides, other action maps, stale sources and nearly vertical
head directions cannot request a turn. Recenter and pawn replacement invalidate
old requests. `BodyYawDiagnostics=1` enables bounded native-state logging; it
also defaults off.

The grass correction requires shader reflection to identify the used
`ViewportShaderParameterProvider._GrassCylindricalBillboardMatrix`, at byte 512
of the captured 1008-byte provider. Unknown layouts and invalid matrices are
left untouched. Only four horizontal matrix elements change. Recycled buffer
and shader identities are invalidated at creation. A world-facing target comes
from the rendered head direction minus physical LOCAL-space head yaw. This
avoids subtracting a new pose from an older native camera matrix. Repeated
application is idempotent; the last target remains available between XR frames.
The physical anchor survives the mod's F8/controller recenter.

The companion [shoulder fix](SHOULDERS.md) removes a hemisphere sign flip in
upper-arm placement. That correction also applies when body yaw is disabled.
A passive `native-save-ready` log marker supports the optional load helper.

## CPU tests

From the repository root, with a C++17 compiler and CMake installed:

```sh
cmake -S mod/tests -B build/body-yaw-tests
cmake --build build/body-yaw-tests
ctest --test-dir build/body-yaw-tests --output-on-failure
```

For a multi-configuration generator, add `--config Release` to the build and
`-C Release` to CTest. Tests cover yaw signs/wrapping, stale and repeated
sources, owner/reference changes, recenter, camera lag, additive manual turns,
four full physical turns, grass matrix invariants and shoulder placement.
Existing upstream tests remain registered. On Windows, this also builds
`native_input_probe.exe`, restricted to inspecting the visible Far Cry 2
window, sending Enter to it and capturing its client area.

## Native tests (Windows)

Prerequisites: an already-installed source-built mod, DirectX 10, a working
32-bit OpenXR runtime, a Continue save and the supported Steam image. The
scripts do not install the mod, patch an executable or configure the runtime
globally. Use a test profile/save and close the game before starting; gameplay
may perform its normal autosaves. Output directories must be fresh. Keep
generated manifests, logs and captures private: they contain local paths and
may contain game/shader data. Default `build/` output is ignored by Git.

Build the normal DLL with `TC=<llvm-mingw bin> sh mod/build.sh`. Preserve it,
then build a separate fixture DLL with `BODY_YAW_TESTING=1` in the environment.
Each build overwrites `mod/build/dinput8.dll`; copy the fixture to, for example,
`build/body-yaw-fixture.dll`. Production excludes both test environment adapters
(`FC2VR_BODY_YAW_TEST_FILE` and `FC2VR_GRASS_CAPTURE_DIR`).

The following PowerShell example uses local variables for installation-specific
paths. Set `$gameBin`, `$profile` and `$runtimeJson` to existing paths; set
`$probe` to the built `native_input_probe.exe` (under `Release` when applicable).
No external probe, reference images or private launch template is needed.

```powershell
& mod/tests/new_native_trial.ps1 -GameBin $gameBin -Profile $profile `
  -RuntimeJson $runtimeJson -Output build/body-yaw-trial
$env:FC2VR_GRASS_CAPTURE_DIR = "$PWD/build/body-yaw-gpu"
New-Item -ItemType Directory $env:FC2VR_GRASS_CAPTURE_DIR
try {
  & mod/tests/run_body_yaw_native.ps1 -FixtureDll build/body-yaw-fixture.dll `
    -Probe $probe -Manifest build/body-yaw-trial/test-manifest.json `
    -Output build/body-yaw-native -Enabled 1 -GrassFix -GrassMotion
} finally {
  Remove-Item Env:FC2VR_GRASS_CAPTURE_DIR -ErrorAction SilentlyContinue
}
& mod/tests/check_body_yaw_native.ps1 -Run build/body-yaw-native `
  -Enabled 1 -GrassRecenter
python mod/tests/check_grass_gpu.py build/body-yaw-gpu --enabled --recenter --motion
python mod/tests/test_grass_gpu_rejections.py build/body-yaw-gpu
```

The runner temporarily deploys the supplied fixture and settings, backs up and
restores the installed mod DLL, INI and profile, checks original binary hashes,
and terminates only its owned game/writer processes. It limits affinity to 16
available cores, uses process-local `XR_RUNTIME_JSON`, and sets RenderScale=0.5
for enabled and disabled comparisons. Bounded Enter input selects Story/Continue;
fresh native on-foot player and camera evidence is required before assertions.
`-BodyOnly` disables weapon rendering, controller aiming and skeleton dumping
to check that body yaw is independent of those options. For a disabled baseline,
use a fresh output with `-Enabled 0`, omitting grass options; check it with
`-Enabled 0` as well.

The fixture CSV is `pid,sequence,yaw,pitch,roll,tracked,allowed,reset` in degrees.
Only the matching PID consumes it; repeated sequence numbers expire after
100 ms. It replaces head/eye poses, never controller actions, and retains real
game-mode gates. `test_body_yaw_writer.ps1 -Output <fresh directory>` exercises
atomic replacement, freezing and recovery without launching the game.

Native checks inspect character/pawn state and both engine eye cameras during
transitions and after settling, including stale sources, recovery, tracking
loss and recenter. The ineligible phase checks a request lease, not a real
vehicle transition. GPU checks inspect actual bound upload bytes for both eyes:
`N` records are independent native uploads, `B` records are buffer contents and
`X` records contain frame poses; draw records identify buffer use. Checks require continuous pose variation and
byte-for-byte preservation outside the four facing floats. Negative checks
inject native spin, projection damage and missing-eye coverage into captured
data to verify rejection. These checks do not measure headset comfort.

## Manual testing and remaining coverage

With the production DLL installed, create a fresh trial inventory as above,
then run:

```powershell
& mod/tests/launch_and_load.ps1 -Trial build/body-yaw-trial -Probe $probe
```

This uses real tracking, refuses synthetic pose input, sends at most five
confirms, verifies a fresh native readiness marker and leaves the loaded game
running. `-CloseAfterLoad` closes its owned process after verification instead.
It does not send save commands. Native readiness works with an unworn HMD;
it is not visual acceptance.

Physical testing on SteamVR with an RTX 2060 accepted body rotation, continuous
grass stability, shoulder placement through large turns and automatic loading.
To reproduce, turn physically left/right past 90 degrees, look behind, cross
the +/-180 boundary and complete a full turn. Check shoulders and hands, world
stability, pitch/roll, walking/running/stopping, combined stick/head turns and
recenter. Pause/resume, reload and verify heading and movement again.

GOG is unsupported by the new adapter. Real OpenXR reference-space rebasing,
device-loss recovery, actor reuse across saves/respawns, scope/vehicle/mounted
transitions and all foliage materials still need broader testing. Do not infer
coverage of those paths from synthetic request-lease or reset phases.
