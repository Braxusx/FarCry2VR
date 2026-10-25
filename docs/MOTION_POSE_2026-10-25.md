# Motion-controller pose fixes — offline work

Branch: `codex/tracking-arms-physical-steering`. Starting checkpoint: `5fdfaa4`.
No runtime launch, deployment, packaging or push was performed for this work.
Local R2 commit: `268e063`; local R3 commit: `aaf7fda`. The final R8 commit also
records combined tests/build. Published remote checkpoint remains `5fdfaa4`.

## R2: continuous fractional twist

Hypothesis: wrapping the relative arm rotation before fractional weighting creates
a branch discontinuity. Control: the previous formula jumps almost 180 degrees
at weight 0.5 when input crosses 179.9 to 180.1 degrees. Variable: continuous
per-arm angle history before applying the existing weight. Decision: synthetic
crossings must remain continuous and repeat calls for one sample must be identical.

`common/arm_pose.h` now rejects degenerate projected angles, keeps a hinge row per
observed rig/context, unwraps the measured angle and advances once per source
sequence. Correction is saturated at one full turn each way, with anti-windup;
this avoids unlimited anatomical winding and responds immediately when reversed.
The existing rotation remains about the solved upper-arm axis, leaving the hand
and weapon target calculation unchanged. It is not a new IK or skinning solver.

History resets on observed pawn, skeleton, palette, bone count/landmarks, weapon,
control mode, input/hand epoch, lost authority or a >100 ms source gap. It rejects
older samples. Pointer identities are observations, not native lifetime/ABA proof.
Continuity cannot infer rotations over 180 degrees between samples, and a new
context deliberately starts on the principal branch. Degenerate measurements skip
correction rather than inventing a zero twist. Named rig/corrective ownership and
weighted-mesh acceptance remain separate gates.

Three new pure groups pass MSVC x86 Release and GCC i686 with warnings as errors:
twist 13,620 checks, reset 33, projection 1,447 (15,100 total). The existing 19
groups passed during implementation. Full integration build and final combined
test evidence are recorded below when completed. Runtime/headset acceptance has
not been performed.

## R3: stable support socket

Hypothesis: using the live animated left wrist as the two-hand lever lets weapon
animation steer fixed controllers. Control: changing that wrist changes the old
lever. Variable: freeze the left-to-right wrist relation from the existing accepted
normal hold. Decision: changing animation cannot change the bound socket/token;
moving the physical right root must place the same local socket in its current frame.

`common/weapon_pose.h` owns rigid frame composition, a support descriptor and grip
latch. The descriptor binds observed pawn/player rig/palette/landmarks, weapon
instance/model/palette/count, existing type key, accepted-hold revision and input
epochs. A new token prevents a model/hold change from inheriting a held grip, even
between render publication and skeleton application. Acquisition is neutral then
a new grip near the current-frame socket; loss/change requires a fresh grip.
Animation cannot rewrite the socket during that binding. The visible left wrist
uses its accepted local pose, while physical controllers steer the rifle around
the right hand. Antiparallel directions have a deterministic axis; coincident
hands cannot establish a steering direction. Existing pistol support placement is
retained. Current-frame controller placement replaces the old cached ref-space point.

This uses the existing calibrated/saved hold, not a newly proved weapon-asset
socket. It conservatively requires a rigid accepted steady hold, both physical
hands, normal on-foot controls and an observed weapon model. Until ready, support
grabbing is unavailable and the free-hand route remains. Map, phone, machete and
vehicles do not acquire a weapon-support lease. It does not fix persisted hold
keys, model lifetime/ABA, native job scheduling, all attachment caches or rig roles.
No new native function/offset is introduced. Existing readable-pointer probes are
not lifetime guarantees. Real weapon/mesh fit and native scheduling need runtime
and headset acceptance. The source syntax check also caught and corrected a
Windows `near` macro collision which the portable tests alone could not expose.

## R8: basis-correct hitscan spread

Hypothesis: a native world-axis deviation added to a tracked central ray does not
rotate with the gun. Control: the previous residual formula fails a 90-degree roll.
Variable: express the complete native direction in the unposed native camera basis
and reconstruct it in the accepted weapon/scope basis. Decision: local asymmetric
spread and cone angle must survive arbitrary yaw/pitch/roll and common world motion;
identical bases must retain the native shot, and zero spread must follow the ray.

`TransportSpread` validates finite rigid, non-reflected frames and nonzero input.
The shot record now carries both full bases under the same lock as its original
receipt and weapon identity. The native source uses published raw camera rows,
not the steadied eye frame. The target uses the same composed/captured weapon
frame as the central sight ray. A scope override carries the actual rendered
camera's right/up axes, including the scope's existing no-roll policy, rather than
silently retaining weapon roll. Original age/owner checks remain. Invalid geometry
leaves the original native shot untouched. Mounted-gun routing is unchanged.

This changes hitscan direction transport only; it does not replace native random
spread, recoil, ammo, damage or projectile simulation. Source-basis timing versus
the actual native shot, native hook/job scheduling and impact/barrel agreement
still need a controlled game run. No new native call, address or hook was added.

## Final offline evidence

All 29 MSVC x86 Release CTest groups pass (`/W4 /WX`): 26 numeric groups with
62,986 explicit checks plus three source-wiring guards. The nine new numeric
groups (31,761 checks) also pass GCC 16.2.0 i686 with
`-O2 -Wall -Wextra -Werror`:

| Group | Checks |
| --- | ---: |
| twist | 13,620 |
| reset | 33 |
| projection | 1,447 |
| socket | 2,506 |
| ownership | 41 |
| latch | 56 |
| support arc | 1,088 |
| spread | 12,963 |
| invalid spread | 7 |

Separate test executables were rebuilt against ignored copies of the final
production headers, deliberately restoring principal-angle weighting, per-call
socket renewal and world-axis residual transport. Twist, socket and spread runs
fail with exit 1 (1,206, 1,500 and 5,506 failed checks respectively). These mutants
were never compiled into the DLL. Source-wiring guards are text/topology checks,
not native execution or concurrency proof.

The full proxy builds with installed GCC i686 using the existing build source and
library list. Development artifact: `build/motion-pose-1/dinput8.dll`, 1,259,171
bytes, SHA-256:
`56cb51b97315d3e918b1720c2ba6ec1db7dfc75e1be730b50c80bf60e2de69f3`.
PE32/i386, undecorated `DirectInput8Create` and Windows-only imports were verified.
There are 31 pre-existing warning diagnostics versus 32 at the parent; removing
the old wrapped-twist line removed one warning. No new warning text was introduced.
The documented llvm-mingw route remains unverified; the launcher and DLSS helper
were not rebuilt. Only process-local compiler PATH setup was used. Build/test logs
and controls are ignored under `build/motion-pose-1/`.

## Acceptance, limitations and rollback

These are offline implementations of R2/R3/R8 mechanisms, not acceptance of all
arm/weapon jank. R1 publication/consumption timing, R4 full native lifetime handling,
R6/R7 torso/IK/weighted-corrective binding, R9 unknown-image refusal and R10 saved
hold/attachment/reload ownership remain open. In particular, binding an existing
saved hold to an observed model does not prove that hold's asset provenance; the
legacy type-key and pointer-only hold caches have not been redesigned. This is not
a DQS implementation or a promise that wrist pinching is visually solved.

Next authorized game/HMD run should verify the `motion-pose-1` module stamp, source
consumption sequence, wrist roll through the former wrap boundary (both arms),
fixed-controller weapon animation, rifle/pistol regrips, weapon/model changes while
gripping, recapture, focus/dropout/recenter recovery and phone/map/reload regression.
Compare yawed/rolled hitscan impacts with native spread controls and the rendered
scope. Close only the owned game process after the run. None of those runtime or
headset gates was exercised here.

Rollback the three local commits as a set to `5fdfaa4`, or revert R8 then R3 then
R2. No deployed files or runtime configuration need restoration. No GitHub push,
PR, packaging or release was performed for this batch.
