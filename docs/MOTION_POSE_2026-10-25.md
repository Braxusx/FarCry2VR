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

## Remaining in this offline batch

Stable support binding (R3), then basis-correct bullet spread (R8), followed by a
full development DLL build and combined regression run. Neither is claimed fixed
by the R2 checkpoint.
