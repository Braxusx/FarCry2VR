# Motion-controller pose fixes — offline work

Branch: `codex/tracking-arms-physical-steering`. Starting checkpoint: `5fdfaa4`.
No runtime launch, deployment, packaging or push is authorized for this work.

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

## Remaining in this offline batch

Basis-correct bullet spread (R8), then the final combined regression and full DLL
build. These are not claimed complete by the support checkpoint.
