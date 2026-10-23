# Tracking, arms and physical vehicle steering — implementation plan

Date: 2026-10-23. Proposed contributor work against
`92a29c5b1e28a652d067262aacc59b421ed044c1` (1.0.1 / v230).
Local source and GitHub main matched when this plan was written. Rebase the plan's
source locations against newer work before implementation. The plan is checkpointed
on `codex/tracking-arms-physical-steering`. R5's position/direction boundary and
R1's shared effective camera frame and the first R4 input-authority safety slice
are now built and desk-tested.
Runtime acceptance has not started. See [contribution status](CONTRIBUTION_STATUS.md).

## Current checkpoint

- R5 implemented offline: `HeightOffset` translates positions only, not eye or
  culling directions. Existing arm/calibration, flashlight and monocular directions
  now use the same pure helper. Position origin subtraction and world scale retain
  their previous behavior.
- R1 implementation now prepares a single immutable frame before arm publication
  and any scope/cull/eye posing. Smoothing advances once per application sequence.
  The arm/interaction adapters use its absolute origin, not a later Camera-bone
  origin plus offset. Raw camera rows stay separate for calibration/native aim;
  native flat fallback uses a raw frame consistently. Frame-use diagnostics were added.
- First R4 slice separates held visual poses from physical authority, expires raw
  samples after 100 ms, stamps focus/lifecycle/hand epochs, requires neutral
  recovery and cancels transient gestures without synthetic releases. Aim/scope,
  contacts and cached NPC drive retain source evidence. Accepted throw intents
  keep a separate bounded native-animation deadline. See the
  [authority checkpoint](INPUT_AUTHORITY_2026-10-23.md) for boundaries and limits.
- Nineteen CTest groups pass with MSVC x86: 31,225 numeric checks plus two
  source-wiring guards. Numeric groups have passed MinGW GCC x86 as well. A frozen
  original SteadyCam oracle verifies extraction; bob/yaw/bone negative controls
  exercise the old split. Rebuilt expiry/rearm mutants fail the new safety tests.
- Both unmodified baseline and changed x86 DLL build with installed MinGW GCC;
  exports checked, no added warnings. The documented llvm-mingw toolchain is not
  installed, so that build route remains unverified. Nothing deployed or packaged.
- This is a partial PR 0 / PR 1 foundation and first PR 2 slice, not completion of
  any of those PRs. R1 runtime scheduling/latency and HMD acceptance, unknown-image
  refusal and full native session/pawn/model/palette/cache ownership remain open.
  If hierarchy hooks apply the previous sample before viewport publication, resolve
  the clock with a proved presentation seam; do not mask it with bone-origin drift.
  No arm-twist, support-grip, manual-reload, physical-steering or DQS fix is claimed.

## Contribution branch and upstream review

- Repository: `Braxusx/FarCry2VR`; local checkout:
  `D:/Dev Debug/Other VR Mods/FarCry2VR`.
- Working branch: `codex/tracking-arms-physical-steering`, created from the base
  commit above. Keep this work off `main`; upstream `main` remains the release owner.
- Initial checkpoint contains this plan only. Follow it with focused test and
  implementation commits, beginning with PR 0's regressions and then the shared
  effective frame/position-direction fixes. Update this document's status as each
  gate is actually reached; do not label planned work implemented.
- When ready to sync, push this contribution branch to `origin` and open a review
  against `main` with scope, tested build/configuration, remaining acceptance work
  and per-change rollback instructions. Publishing the branch/PR is a separate
  step from creating this local checkpoint.
- Before syncing, inspect upstream changes and integrate relevant `main` updates
  deliberately. Preserve Braxusx's work and user settings; avoid force-pushing a
  branch another contributor has started using.
- For a staged merge, create small review branches/PRs from each accepted parent
  checkpoint so each diff contains only its intended fix. This working branch can
  retain the combined integration candidate. Braxusx decides which changes merge
  and when they enter a release.

## First usable milestones

**M1 — stable tracked arms and weapons:** walking/sprinting and head-only movement
do not move a stationary tracked weapon unexpectedly; wrist roll is continuous;
support grips stay attached through normal animation; focus/tracking loss and
weapon changes cannot retain stale control. Existing phone/map/reload, scopes,
holsters, native gamepad controls and stereo rendering remain usable.

**M2 — physical steering in one proved vehicle:** grab the actual steering wheel
with either hand, turn it to steer through the existing native input path, add or
release the second hand without a steering jump, and retain acceleration, braking,
handbrake, seat change, exit and a usable controller recenter action. The modelled
wheel and visible hands agree with steering. Stick steering remains available.

M2 is one measured vehicle/profile first. A second contrasting wheel/vehicle proves
profile reuse before claiming fleet-wide support. Boats with different controls,
mounted guns and the paraglider remain separate modes.

## What the source establishes

The October 23 source review reproduced seven arithmetic/control properties in
an isolated x86 harness with 13 assertions. These establish mechanisms, not their
frequency or causal share in a headset session:

| ID | Source and defect | Fix owner |
| --- | --- | --- |
| R1 | `main.cpp:2511–2513` publishes raw camera basis for arms; `2575–2576` steadies only the stereo basis | Shared effective pose frame |
| R2 | `arms.cpp:1115–1133` wraps twist to ±pi before fractional weighting; 0.2° boundary crossing can produce ~180° output jump | Arm deformation |
| R3 | `arms.cpp:927–957` derives support steering from the current animated left hand; `main.cpp:2355–2369` grip latch lacks weapon/epoch identity | Weapon support lease |
| R4 | `xr.cpp:343–429` promotes held poses to valid and publishes no age; bullet hook retains unexpired `g_shotValid` | Pose/input authority |
| R5 | `main.cpp:386,2687–2689` adds height translation to directions as well as positions | Math boundary |
| R6 | `arms.cpp:978–982,1098–1108,179–192` uses head yaw for shoulders and lacks robust vertical/pole/opposite-axis handling | Torso and arm solve |
| R7 | `arms.cpp:138–158` assigns external helpers by coincident positions rather than named anatomical role | Rig binding |
| R8 | `arms.cpp:2759–2762` adds native spread residual in world axes instead of transporting it with the gun | Gameplay aim adapter |
| R9 | `gameaddr.cpp` selects by timestamp/size and falls back to Steam addresses on unknown images | Native access profiles |
| R10 | Pointer-only hold caches, animated topology-derived type keys and HUD-reticle-keyed clip maxima can outlive or misidentify their owner | Weapon identity and reload presentation |
| V1 | `controls.cpp:207–226` sends stick steering to `sThumbLX`; no wheel gesture exists | Physical steering adapter |
| V2 | The driver map assigns left grip to handbrake and right grip to seat change | Input ownership/bindings |

The current action-map reader distinguishes driving, passenger/mounted and on-foot
contexts, but `VehicleMode()==1` does **not** establish the vehicle instance, seat
generation, wheel geometry or native steering state. Those are explicit discovery
requirements, not inferred fields.

## Architecture: extend narrow seams

Keep the existing engine viewport rendering, D3D10/D3D11 bridge, XInput adapter and
native gameplay systems. Extract pure policy/math only as it is needed by each PR;
do not make a large rewrite a prerequisite.

Suggested additions under `mod/src/`:

| Component | Responsibility |
| --- | --- |
| `common/pose_frame.h` | Explicit position/direction transforms; effective tracking-to-game frame; frame/space/recenter stamps |
| `common/pose_authority.h` | Per-component active/valid/tracked state, freshness and neutral rearming |
| `common/arm_pose.h`, `common/rig_binding.h` | Hinge solve, continuous twist, named weighted/corrective roles and staged results |
| `common/interaction_lease.h` | Weapon-support and wheel-grip ownership/reset rules, physical versus rendered targets |
| `common/vehicle_steering.h` | Pure grip-to-angle state machine and normalized steering intent |
| `vehicle_context.{h,cpp}` | Verified native driver/vehicle/control identity and profile resolution; observation first |
| `vehicle_controls.{h,cpp}` | Context-specific input arbitration and one immutable steering result per sampled update |
| `vehicle_profiles.*` | Control kind, model identity, neutral control frame, geometry, limits and input response |

Names are proposed, not a frozen API. Pure modules contain no native pointers, D3D
or OpenXR calls. Reuse the existing critical-section publication pattern, publishing
coherent copies and releasing locks before engine callbacks. Native adapters retain
ABI, image, thread, lifetime and range validation.

Every shared sample carries application frame, sequence, session, reference-space
generation, recenter epoch, predicted time and monotonic receipt time. Head and each
hand retain separate position/orientation validity and tracking status. Predicted
display time is not elapsed publication age. A cached visual pose has a distinct
status and cannot authorize aiming, physics or wheel input.

Keep three values explicit: physical controller input, accepted interaction state,
and rendered hand/weapon/control pose. Rendered attachment output never becomes
fresh control input. Advance gesture/torso/twist/steering history once per sample,
not once per eye or each time XInputGetState is polled.

## Proposed PR sequence

| PR | Scope | Dependencies | Completion proof |
| --- | --- | --- | --- |
| 0 | Portable regression harness; build identity and diagnostic stamps; supported-image dependency census | Baseline | Reproduce reviewed failures with controls; clean x86 DLL build on the documented llvm-mingw route; unsupported image cannot activate new native lanes |
| 1 | One effective frame for eyes/arms/weapon/rays; distinct position/direction conversions (R1/R5) | 0 | Fixed-controller camera-bob fixture has no frame disagreement; nonzero height offset cannot rotate a direction; runtime walking/head-only checks |
| 2 | Freshness, tracked status, lifecycle invalidation, input/hand ownership and exact-image refusal (R4/R9) | 0–1 | Dropout/focus/recenter/swap tests revoke stale authority; fresh non-pose buttons still work; unknown images cannot fall through to Steam calls; no replay on repeated input polls |
| 3 | Named rig roles, continuous twist, robust hinge/poles, torso heading (R2/R6/R7) | 1–2 | No branch flip/NaN; length/endpoints retained; actual weighted mesh improves; both hands/weapon targets retained; M1 arm gate |
| 4 | Stable support socket and identity; basis-correct spread; scoped reload/attachment presentation (R3/R8/R10) | 1–3 | Fixed controllers plus changing animation cannot steer gun; held-through-swap grip cancels; roll/yaw impact controls; phone/map/reload regression; M1 complete |
| 5 | Observe driver/vehicle/control identity, actual wheel geometry/state and input response | 0–2 | Driver/passenger/exit/load controls distinguish identities; log-only profile binding; no steering writes |
| 6 | Physical wheel core and native XInput axis integration; one-hand, two-hand and binding arbitration | 1–3,5 | Synthetic moving-vehicle/angle/handoff tests; bounded in-game steering input and same-wheel/hand visual proof; M2 first vehicle |
| 7 | Second contrasting vehicle, profile persistence/validation, launcher controls and full transition/comfort regression | 4,6 | No solver code change for second profile; settings round-trip; supported/unsupported vehicle and legacy-mode controls; M2 reusable implementation |
| 8 | Optional DQS deformation experiment | 3 | Exact-mesh comparison shows benefit beyond corrected linear skinning; separate renderer integration proof before enabling |

Vehicle observation and pure steering math can progress while weapon support is
being corrected. Physical steering depends on the shared pose/lifecycle foundation;
the DQS experiment is not on its critical path. No elapsed-time promise is made for
unresolved native vehicle/control discovery.

## Stable pose and arm implementation details

1. Build the effective camera/tracking frame once, applying `SteadyCam` exactly
   once. Use it for controller world targets, eyes, reticle and interaction origins.
   Retain raw native camera data separately for animation and native spread analysis.
   Verify that game-thread consumption and both render eyes use the intended stamp;
   publication alone does not prove when the final palette consumes it.
2. Expire or invalidate shot, support and object leases on focus/owner/epoch loss.
   Clear failed head validity. Handle OpenXR reference-space changes at their
   effective time, rebasing/resetting consumers coherently. Ordinary fresh buttons
   survive position-tracking loss; focus loss or stale publication neutralizes them.
3. Resolve the actual runtime name hash and parent topology. The exact audited FC2
   assets identify UpperArm → Forearm → Hand, weighted twist helpers, and separate
   Pelvis-parented Elbow correctives. Validate against the current runtime rig;
   neither a blanket index-minus-one conversion nor another bone-bumping session
   is the intended discovery method.
4. Solve endpoint/hinge positions first. Distribute axial pronation through the
   existing authored deform chain with continuous branch state. Use deterministic
   opposite-direction axes, previous/rest pole fallbacks and bounded elbow-circle
   constraints. Torso heading must not track every head-only look; seated drivers
   require their own shoulder/root context.
5. Stage all affected records before writing, validate finite bases and identities,
   and identify any later native overwrite. Treat elbow-cap deformation separately;
   neither full-forearm nor upper-only carry is assumed visually correct.
6. Store a supported model-local weapon/support binding; distinguish definition,
   instance and rig generation. Preserve physical support steering and derive the
   visible support wrist from the accepted weapon socket. Aim/spread uses the same
   accepted weapon frame; native gameplay retains ammo, recoil and action ownership.
7. Scope attachment/reload-pose retention by owner and bone-cache generation/count.
   An observed clip increase or timeout is not proof of native reload acceptance or
   presentation completion. Keep the ordinary reload route working; full manual
   reloads remain a separate feature milestone.

## Physical steering design

### Native binding and profiles

First prove the current driver pawn → vehicle entity → seat/control relation for
each supported image. Record full module hash and file size, receiver/subobject,
pointer width, calling convention, concrete callees, field producers/consumers and
lifetime. Prefer existing native accessors or the existing input consumer. The
action-map name alone is only a mode signal.

A `VehicleControlProfile` identifies a real vehicle/control definition and model
variant, with:

- control kind (`Wheel`, future `Handlebar`/`Tiller`, or unsupported);
- vehicle-local neutral control frame, hinge axis, rim radius and grab depth;
- steering sign, visual angular limits, normalized input mapping/deadzone;
- grip/release geometry, tracking-age and angle-rate limits;
- named wheel/attachment binding and seated hand/root policy;
- schema version and provenance/calibration revision.

The current instance/seat generation is separate from that reusable profile.
Unknown or ambiguous bindings use stick steering. Do not identify a vehicle from
bone count, camera proximity or a driver action map alone. Do not ship invented
wheel transforms. One-car calibration may be a temporary explicit measured profile;
it must be stored against a real definition/model identity, not world coordinates.

Prove whether `sThumbLX` is a desired steering amount or a rate-like input under
the game's settings, including deadzone, response and return behavior. Start with
the existing `controls.cpp:210–211` sink; do not write vehicle physics or wheel bones
as the control mechanism. Native wheel animation/readback is separately correlated
with the input to establish the visible control angle.

Use chassis/seat orientation for the control geometry, never headset yaw. Free
head look and any vehicle-camera comfort transform must not change steering with
the hands fixed to the cockpit. Reuse accepted vehicle camera behavior while
testing acceleration, bumps, reversing and turning for discomfort.

### Grip and angle computation

Use positional manipulation around the wheel, inspired by VR Expansion Plugin's
`VRDialComponent`: capture in the parent's local frame, calculate signed angular
change, handle wrap/limits and release when out of range. This is transferred
interaction math, not an Unreal runtime dependency.

1. Transform the physical GRIP position into the **neutral vehicle-local control
   frame** at a coherent stamp. Do not measure against the already-rotated wheel
   bone, or its own motion will feed back into steering. Common vehicle translation
   and rotation must cancel when a hand stays fixed relative to the cockpit.
2. Require a new grip edge near the rim/plane with active, valid, tracked poses.
   Capture hand angle and current accepted wheel angle without snapping to zero.
   Points near the hinge have no reliable angle and cannot establish/advance a grip.
3. Project onto the wheel plane; compute `atan2` and unwrap signed incremental
   angles. Clamp steering limits with anti-windup so reversing from a stop responds
   immediately. Bound implausible time gaps and angular jumps; do not interpret
   recentering or a vehicle identity change as a steering stroke.
4. One hand is sufficient. Adding a second hand rebases it at the current accepted
   angle. Combine continuous per-hand angle estimates with explicit confidence;
   if they disagree materially, retain the primary estimate and reject/rebase the
   secondary instead of oscillating between writers. Releasing either hand rebases
   the remaining owner without a jump.
5. Map one accepted angle into one normalized XInput steering intent. Produce it
   once per source update; repeated gamepad queries read the same result. The engine
   retains vehicle simulation, tire response and its own steering return behavior.
6. While held, visible hands attach to wheel-local contacts using the accepted/native
   control angle; raw physical controllers continue steering. Render both eyes from
   one accepted contact state. Account explicitly for the seated torso and native
   vehicle arm animation rather than applying on-foot shoulder assumptions.

### Input ownership and release

In the existing driver map, a physical wheel grab would otherwise send **handbrake
or change-seat**. Resolve ownership before emitting any gamepad buttons.

Proposed prototype policy, enabled only with physical steering:

- Right/left triggers remain accelerator and brake/reverse. A remains seat change;
  X remains exit. Grip presses accepted by the wheel are consumed.
- Left-stick click becomes the explicit handbrake while this driver mode is active.
  Both stick clicks held become the driver recenter chord; consume conflicting click
  actions when recognizing that chord. On-foot and legacy vehicle bindings stay as
  they are. All changed bindings must be shown in the launcher and controls guide.
- Grips released/cancelled by tracking loss remain consumed until a fresh neutral
  state; an already-held grip must not suddenly become a seat-change/handbrake press.
- Physical steering owns the axis while grabbed. A deliberate stick movement takes
  over and cancels the wheel lease; never sum or choose the larger of physical and
  stick commands. With no wheel lease, the existing stick path owns steering.
- On ordinary release, return to fresh native stick input. On lost tracking,
  stale context, menu, passenger/turret, seat switch, exit, death or load, cancel
  physical steering immediately. Focus loss/stale input also releases throttle and
  buttons. Do not retain last steering/throttle as a supposed safe fallback.
- On return from a transition, require neutral then a new grip. Recenter changes
  the reference epoch and cannot silently retake the wheel while held.

These proposed driver-only bindings are a product decision to validate in M2,
not an existing behavior or a requirement to change every controller scheme.

`PhysicalSteering` starts off by default. A validated profile is required even when
enabled. The first version is a real cockpit-wheel interaction, not an invisible
head-relative air wheel. Unsupported vehicles retain their native stick controls.

## DQS decision gate

DQS is promising for residual wrist volume collapse, but cannot repair wrong bone
ownership, stale frames or discontinuous rotations. FC2's actual skin shader must
be identified before claiming implementation feasibility from the current palette
hook alone. A Blender deformation option does not select the game's skinning method.

Use the exact mesh/weights and identical pose sweeps to compare current LBS,
corrected-twist LBS and corrected-twist DQS. Measure wrist radius/volume, endpoint
and seam continuity, elbow bulging, normals and cost. Include flexion combined with
pronation, not just a straight wrist. Preserve scale/shear behavior where used.
If DQS wins, prototype a tightly classified arm-only deformation path with matching
depth/shadow/material passes; prove both-eye output and fallback before enabling.
Retain the existing authored twist bones; virtual SS2 bones/coefficients are not
automatically appropriate for FC2.

## Verification and contributor workflow

Each PR gets focused tests against its production pure functions, with at least
one discriminating old-behavior/negative control. Do not ship tests that only repeat
the implementation or assert the known bug. Keep an x86 DLL build using `mod/build.sh`;
a separate portable CMake/CTest harness need not change the production toolchain.

| Gate | Required controls |
| --- | --- |
| Pose frame | translated/rotated game frames; nonzero height; asymmetric/canted eyes; both eyes consume one stamp; walking raw/effective-basis comparison |
| Lifecycle | valid versus tracked; focus loss; stale/duplicate/regressing sequence; reference-space event; recenter; same address with new owner generation; repeated input polling |
| Arms | exact opposite directions; near-straight/bent/overhead reach; ±pi roll crossing; independent head yaw; fixed hand/weapon endpoints; named-bone mismatch refusal |
| Weapons | fixed controllers with changing animation; one/two hands; swap while grip held; phone/map/ADS/reload; native ammo and final barrel/impact regression |
| Steering math | any rigid cockpit transform; angle wrap; one/two-hand handoff; near-hinge refusal; limits/reversal; irregular cadence; no accumulated state changes on duplicate reads |
| Steering modes | driver versus passenger/turret/map/repair/loading; enter/exit/seat switch; moving vehicle; recenter while gripping; tracking/focus loss while accelerating; unsupported profile |
| Input conflict | wheel grip never emits seat change/handbrake; explicit buttons still work; recenter chord; deliberate stick takeover; stale release cannot replay a press |
| Integration | matched module/DLL identity; actual consumed bone/attachment/control state; both-eye captures; existing rendering/features preserved |

Add transition-gated and time-throttled proof lines: `pose_frame`, `pose_authority`,
`arm_pose`, `weapon_support`, `vehicle_bind` and `vehicle_steer`. Include source frame,
owner/epoch, physical-versus-visual targets, rejection reason, grip owner, angle and
emitted axis. Log counters are evidence of code execution, not visual acceptance.

Settings changes include parser, defaults, launcher controls, binding guide and
round-trip validation together. Check launcher fixed-capacity settings storage
before adding entries. Preserve user offsets/settings; do not silently retake or
rewrite all saved holds. New experimental native lanes need an off switch, a proof
line and exact-image gating before first mutation.

Keep PRs small and independently revertible; split mechanical extraction from
behavior where practical. Each behavior build identifies its version/commit and
binary hash. Coordinate shared-file edits through Git review; a collaborator role
does not establish that another person's working tree is idle. No unrelated
graphics, packaging or performance changes in the pose/steering PRs.

Runtime and headset tests require a named scenario/build, explicit current runtime
ownership and a bounded checklist. Close only the test-owned FC2 instance afterward,
prefer graceful exit and verify it stopped. Offline tests do not promote runtime or
headset acceptance. Use the existing title/save/scene controls before diagnosing
an unrelated launch failure as a feature regression.

## Reference implementations and evidence boundaries

- Source baseline: [Braxusx FarCry2VR](https://github.com/Braxusx/FarCry2VR/tree/92a29c5b1e28a652d067262aacc59b421ed044c1).
- VR Expansion Plugin: [pinned VRDialComponent](https://github.com/mordentral/VRExpansionPlugin/blob/fcbe552ca65e46fd263b6169d01be3021e0b37bc/VRExpansionPlugin/Source/VRExpansionPlugin/Private/Interactibles/VRDialComponent.cpp#L146).
  Inspected local-parent position projection, signed angle delta, grip capture,
  limits and break-distance handling. Its direct-hand-rotation alternative is
  not the selected cockpit-wheel policy.
- In-house FarCry2-vr: exact asset/native name-parent correlation, weighted helper
  ownership, finite staged arm math and reload request/accept/commit separation.
  Latest elbow-cap quality remains open; its deformation policy is not an accepted
  replacement merely because its unit tests pass.
- Latest SS2VR m139 source: shared current-frame accepted weapon root and sockets,
  tracked/fresh identity checks, continuous twist, prepared CPU arm pose shared with
  equipment, and scoped palette restoration. Relevant source hashes were checked
  against the sealed package. New m139 equipment-follow headset acceptance is still
  pending; KEX layouts and generated twist-bone weights do not transfer to Dunia.
- VR playbook: A2 pose publication/recenter; chapter 02 hinge-versus-twist,
  physical-versus-visible hands and snapshot ownership; chapter 12 torso evidence;
  CAM-020 shared render clock; HAND-019 interaction/physics ownership.
- DQS: [Kavan's author reference](https://users.cs.utah.edu/~ladislav/dq/index.html)
  and [Disney's skinning-artifact comparison](https://studios.disneyresearch.com/2016/07/11/real-time-skeletal-skinning-with-optimized-centers-of-rotation__trashed/).

The October 23 detailed audit and raw extracted-source fixtures are retained by the
contributor's in-house project. PR 0 should bring the portable relevant regressions
into this repository so reviewers can reproduce them without private paths or
copyrighted game assets. Native/asset-specific evidence stays outside public Git;
commit only original tests, code, profiles that are safe to distribute and findings.
