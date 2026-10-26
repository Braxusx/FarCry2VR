# Contributor status — tracking frame fixes

Date: 2026-10-26. Branch: `codex/tracking-arms-physical-steering`.
Base: `92a29c5b1e28a652d067262aacc59b421ed044c1` (upstream 1.0.1).

## Latest checkpoint: scenario-runner-1 (local only, debug-only)

Added bounded weapon-swap, tracking-loss/rearm and wrist/reach plans. A compile
gate plus explicit process opt-in are required; raw synthetic inputs pass through
normal caches/rearm/ownership checks. Completion requires downstream observations
and matched, hashed stereo captures, not elapsed time or a requested action alone.
The runner suppresses synthetic calibration learning and clears transient caches
on entry/exit. Native image, palette/thread lifetime and visual acceptance are
still unproved; no game was launched.

All 57 CTest groups pass: 114,095 C++ assertions, six wiring guards and six Python
receipt-control methods. GCC x86 new groups and three assertion-failing mutants
were checked. Normal/debug x86 DLLs build without new warnings; the normal build
contains no scenario arming payload. Nothing deployed, packaged, pushed or PR'd.
`ArmRigFix=0` stays default. See the
[scenario runbook/evidence boundary](SCENARIO_RUNNER_2026-10-26.md).

## Prior checkpoint: owner-gate-1 (local only)

R9 now refuses native initialization/addresses without an exact full-file identity
and matching mapped PE headers (`1a5bc7a`). Only the measured Steam/Definitive
image is enabled; GOG remains gated until its exact image/table binding is verified.
The system DirectInput proxy continues forwarding on refusal.

The next local slice moves current-weapon observation ahead of model discovery,
independently of hand grabbing, and publishes an expiring observed owner cohort.
Palms/aim/scope, support, reload/attachment presentation, pending throws and cached
NPC drive reject owner changes. Pointer-only hold/rest caches were removed; pose
fire releases/rearms across owner loss. This is not native lifetime/worker proof
or authoritative reload acknowledgement; saved-hold provenance remains open.

All 48 MSVC x86 CTest groups pass: 83,935 numeric assertions plus five wiring
guards. The eight new image/owner groups pass GCC x86. Five rebuilt negative
controls fail as expected. Full x86 DLL builds with no added warnings; development
artifact, acceptance boundaries and rollback are in the
[owner/image checkpoint](OWNER_GATE_2026-10-26.md). Nothing launched, deployed,
packaged, pushed or submitted as a PR. `ArmRigFix=0` remains default.

## Prior checkpoint: arm-rig-1 (local only, deformation default-off)

Adds a named, staged 25-record-per-arm path for wrist-roll distribution and elbow
alignment/correctives. It validates CRC names, topology and native helper anchors;
does not guess helper ownership by position; solves finite hinge/reach geometry;
keeps pole history body-local; and preserves palm/weapon orientation while
distributing axial roll by helper position. The cap follows an explicit hinge-frame
policy rather than wrist roll. `ArmRigFix=0` remains the default; no live INI change.

All 39 MSVC x86 CTest groups pass: 81,977 numeric checks plus four source-wiring
guards. New groups also pass GCC x86; rebuilt broken controls fail. All nine audited
avatar rigs map both arms and pass 36 offline staging cases. The full x86 GCC DLL
builds without added warnings. The synthetic weighted-ring improvement is not an
actual-mesh/headset result. See [arm-rig checkpoint](ARM_RIG_2026-10-25.md).

Everything remains local. No runtime, deployment, packaging, push or PR. New
deformation still needs native palette/worker and visual acceptance; saved-hold
provenance, unknown-image refusal and steering remain open.

## Prior checkpoint: motion-pose-1 (`c657d9f`, local only)

R2 fractional twist now unwraps continuously once per physical source sample,
with observed rig/owner/context resets and degenerate-measurement rejection.
R3 support uses a stable hand-relative socket from the existing accepted hold,
current controller placement and binding-token/neutral-regrip cancellation.
R8 hitscan spread now transports a full native camera-basis direction into the
accepted weapon or actual rendered scope basis, including roll.

All 29 MSVC x86 CTest groups pass: 62,986 numeric checks plus three source-wiring
guards. The nine new numeric groups also pass GCC x86. Rebuilt old-formula mutants
fail for twist, animated socket renewal and world-space spread. The full x86 GCC
development DLL builds with no new warnings (31 existing versus 32 at the parent).
See [motion pose checkpoint](MOTION_POSE_2026-10-25.md) for artifact identity,
controls, limitations, acceptance and rollback.

This is not headset acceptance of the arm rig or weapon alignment. Native timing,
full owner/model lifetime, named weighted/corrective roles, saved-hold provenance
and unsupported-image refusal remain open. No steering, manual reload or DQS was
added. Per the user's current instruction, these commits remain local; no game
launch, deployment, packaging, push or PR. The earlier four-commit branch was
published on 2026-10-24 through `5fdfaa4`; historical notes below describe their
individual checkpoint status.

## Prior checkpoint: input-authority-1 (`5fdfaa4`)

First R4 safety slice is implemented and built offline. Original receipt timestamps,
focus/sync and lifecycle/hand epochs now bound input, physical hand contacts and
weapon/scope rays. Cached visual poses cannot authorize physical actions. Input
loss cancels transient gestures without synthetic releases; recovery requires
neutral rearming and repeated XInput polls do not advance gesture history.
Ordinary fresh buttons/native vehicle controls remain separate from pose-guided
fire and interactions. Accepted throw intents retain a distinct native-animation
deadline without renewing raw pose timestamps.

All 19 MSVC x86 CTest groups pass: 31,225 numeric checks plus two source-wiring
guards. The 477 new authority checks also pass GCC x86; rebuilt expiry and rearm
mutants fail as expected. Full x86 GCC DLL builds with no added warnings.
Details, artifact identity, remaining ownership limits and rollback:
[input authority checkpoint](INPUT_AUTHORITY_2026-10-23.md).

This does not complete R4/PR 2: full native owner/model/palette lifetime handling
and R9 exact-image refusal remain open. R1 scheduling and all runtime/headset
acceptance remain unverified. No game launch, deployment, packaging, push or PR.

## Prior checkpoint: shared-camera-1 (`371c2aa`)

R1's shared-frame implementation is built and verified offline. One immutable
effective camera basis/origin, recenter snapshot and scale now feed eyes/culling,
arm/weapon targets, interactions, flashlight, scope and layer conversions. Raw
camera rows remain separate for animation-axis calibration and native aiming.
Later animated Camera-bone movement no longer reconstructs the published origin.
Native flat fallback retains a raw frame for both its view and hands.

Twelve CTest groups pass with MSVC x86: 30,748 numeric checks plus source-wiring
guards. All eleven numeric groups also pass with GCC x86. The unchanged original
SteadyCam function is retained as a test-only oracle and matches the extracted
policy over 1,000 samples. The x86 GCC DLL builds with no added warnings.

**Not runtime accepted:** skeleton-hook scheduling may consume an older published
sample. New `pose use` sequence/thread/call counters expose that boundary; they do
not fix or certify freshness. Do not accept this absolute-origin change until a
game run checks frame consumption and walking/head-only motion. Details, artifact
identity, acceptance and rollback: [shared camera checkpoint](SHARED_CAMERA_FRAME_2026-10-23.md).

No game launch, deployment, packaging, remote push or PR. PR 0/1 are still not
complete; R2 twist, R3 support gripping, remaining R4/R9 authority/image gates and
steering remain separate work. The documented llvm-mingw route remains unverified.

## Prior checkpoint: height-direction-1 (`a41b3b9`)

R5: the former `ToRef` helper added `HeightOffset` to eye and culling direction
vectors. At an offset of 0.2, even a level forward vector became `{0,0.2,-1}`:
approximately 11.31 degrees of unintended pitch, with a nonorthogonal up axis.

`common/tracking_frame.h` now owns two explicit operations:

- `Position`: subtract the XR recenter origin, rotate into its yaw-only frame,
  and apply height translation once. World scale is still applied by the caller.
- `Direction`: rotate only; neither origin nor height translation is applied.

All old `ToRef` call sites were classified and migrated. Arms, calibration,
flashlight and monocular direction conversions share the same helper instead of
duplicating its math. Default `HeightOffset=0` and position behavior are preserved.
No INI keys, native addresses, hooks or animation/interaction policy were changed.
The startup log identifies `contribution height-direction-1`, the loaded module
path, build date/time and configured height behavior. This is not a release version.

## Offline verification

Production helper tested directly by `mod/tests/test_tracking_frame.cpp`;
no copied implementation or OpenXR/game dependency in the normal test target.

| Group | Checks | Coverage |
| --- | ---: | --- |
| position | 15 | Origin subtraction, positive/negative height, yaw, world scale |
| direction | 41 | Origin/height independence, axes, known yaw, old-formula negative control |
| rotation | 365 | Full yaw sweep, length, dot products, orthogonal forward/up |
| stereo | 15 | Asymmetric eye directions and IPD preserved |
| affine | 300 | Displacement consistency, zero-offset behavior, zero vectors |

All 736 checks passed under MSVC 19.50.35729 (Win32 Release, warnings as errors)
and MSYS2 MinGW GCC 16.2.0 (i686, `-O2 -Wall -Wextra -Werror`). Tests use explicit
checks that remain active in Release builds. A separate test executable was rebuilt
against a test-only legacy helper (height added to directions, unchanged position
behavior): the direction group failed 10 checks, exit 1 as expected. That mutant
was never compiled into the mod DLL.

Both unchanged baseline and changed full proxy DLL compiled with GCC using the
source list, optimization, libraries and linker flags in `mod/build.sh`, substituting
`i686-w64-mingw32-g++` for its documented clang compiler. Both produced the same
32 existing warnings (matching diagnostic text after removing line numbers), with
no new warnings. `objdump -p` confirmed PE32/i386, undecorated `DirectInput8Create`,
and only Windows system imports; no dynamic GCC runtime dependency.

The installed GCC compiler requires `C:/msys64/mingw32/bin` on the build process's
PATH for its child executables. No persistent environment setting was changed.
Documented llvm-mingw build, launcher/DLSS helper rebuild, runtime and headset
acceptance remain unverified. The GCC DLL is a development artifact, not a release
candidate. Neither game launch nor deployment was performed.

Local outputs are ignored under `build/height-direction-1/` (DLL, GCC test binary,
build log, legacy control source/binary/log), `build/tracking-tests-msvc/` (CTest),
and `build/baseline-gcc-r5/` (unmodified baseline DLL/log).

Verified development DLL SHA-256:
`758408e2f9bc9aa15da8275641233063f1f0aaabbfe2320dcc9c75b08a20c632`.
Rebuilding changes the compile-time banner and therefore this hash.

## Next acceptance and rollback

Before upstream acceptance, build with the documented llvm-mingw route. With an
explicitly authorized HMD run, compare identical saved poses/configurations at
`HeightOffset=0`, `0.2` and `-0.2`: height should move the view without tilting it,
skewing the eyes or changing tracked weapon direction. Exercise recenter, head
pitch/roll, asymmetric eye views, scope, flashlight and monocular. Preserve and
restore the user's settings, verify the module stamp, and close the owned game
instance after testing. Passing offline math does not establish visual acceptance.

R1 and the first R4 slice followed this checkpoint; see current status above.
R2 wrist/arm twist, remaining pose ownership, support gripping, vehicle steering
and optional DQS remain open.

R5 is one focused fix commit after the plan checkpoint. Roll back that commit
as a whole if needed; no migration, runtime settings or deployed files need undoing.
No GitHub push, PR, packaging or release has been performed.
