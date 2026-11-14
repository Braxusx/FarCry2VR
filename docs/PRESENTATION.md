# Body, controller and held-gadget presentation

This contribution extends 1.0.4 while retaining its GPU handover, weapon-follow,
sight calibration, turret motion and interaction monitor. The camera, player
skeleton, held weapons, gadgets and their separately queued display tiles must
share a compatible body/controller sample. Correcting just one consumer can
leave the others one update ahead or behind.

## Review map

- `body_pose.h`, `pose_frame.h`, `body_transport.h`, `vehicle_hands.h`:
  match the native render camera to a bounded body-sample history and carry the
  same sample identity/root into hand publication. Seats transport rotation as
  well as position. `VehicleViewMatchNative=1` and `SceneViewMatchNative=1`
  select this path. Clock=2 remains the upstream fallback when scene matching
  is off; it must not apply a second correction to an already matched sample.
- `arms_late.inl`, `arms_overlay.inl`, `late_pose.h`, `support_pose.h`:
  correct draw-time copies for proven local-player skin/world providers and
  map/GPS/phone overlays. Match owner, body sequence, weapon, support mode,
  tracking generation and room-scale epoch. Ambiguous/stale/unmatched providers
  pass through. Both eyes share the correction. Scope/AFW/reload paths bypass
  this late adapter. Native worker output, entity transforms and physics stay
  unchanged. Map clock/health layers use the map mesh's correction too.
- `controller_alignment.h`, `controller_anchor.h`, `xr.cpp`:
  `QuestControllerAlignment=1` requires both an explicit Quest system name and
  that hand's Touch interaction profile. Unknown hardware retains the base
  calibration. Profile changes discard old poses/leases. Optional
  `XR_EXT_palm_pose` supplies a grip-relative palm calibration; unsupported
  extensions/bindings fall back. Base INI offsets are never rewritten. The
  Quest machete roll applies only on the legacy WeaponFollow=0 path; 1.0.4's
  weapon-follow/F1 alignment owns the new weapon orientation.
- `walking_legs.h`, `body_facing.h`, `room_scale.h`:
  optional visual torso follow and foot placement, plus exactly-once accounting
  of native room-scale movement. Native collision/character movement remains
  authoritative. Ground queries stay on the game thread; both eyes consume the
  resulting pose. Explicit upstream Steam native body-yaw mode excludes the
  competing visual body rotation.
- `action_context.h`, `interaction_view.h`, `weapon_identity.h`:
  map briefings permit native locomotion/interaction; passive rides suppress
  on-foot holster/two-hand/throw gestures. Weapon identity comes from the native
  equipped object, not a remembered button cycle. The upstream interaction
  monitor gets first installation priority; the guarded GOG fallback is only
  used if it was not installed. Native reach/obstruction/eligibility remains.
- `arms_gadget_depth.inl`: optional GOG world-depth routing through the native
  setter, including copy-on-write instances. Changing a foreground depth flag
  alone does not establish world occlusion. Unrecognized objects are untouched.
- `view_comfort.h`: bounded visual camera-kick correction with a separate native
  gameplay basis for aiming, interaction and collision. Native recoil state and
  getter results are not zeroed. Diagnostics remain opt-in/bounded.

## Options and assets

`BodyFollowHead`, `WalkingLegs`, `WorldBodyOcclusion` and `RoomScale` remain off
by default. Enable explicitly for testing. Quest calibration is automatic only
for the recognized headset/profile pair and has a launcher checkbox.
`OnFootTrace` and `AutoVehicleTrace` default off.

The pistol support path can mirror verified live finger chains. An optional
`fc2vr_native_pistol_pose.bin` reader accepts separately supplied compatible
poses and otherwise falls back; no authored game-pose payload, captures, saves,
private settings or binaries are included. Public regression rigs are synthetic.
The earlier private authored-pistol acceptance therefore does not establish the
visual quality of the public fallback.

## Native seams and checks

GOG 1.03 entries are guarded before installation: hierarchy worker `10611660`,
skin fill `103DFA70`, world publish `1050D300`, cache `103A5A90`, provider getter
`1043B920`; compass display `1008A520` with its consumer `103A3865`. Addresses
are image virtual addresses, relocated against the GOG image base, not translated
as Steam addresses. Every required entry is preflighted; partial installation
keeps correction inactive. Depth routing additionally verifies its native
command/pass sites. Unsupported editions retain the existing provider path.

The Win32 production-adapter tests exercise skin/world/overlay ownership,
thread-local scratch, repeated getters, original-function/return semantics,
reference changes, recentering, support constraints and native-byte preservation.
The map HUD regression compares mesh and compositor transforms, including the
fractional origin, and rejects a mismatched graphics owner or disabled frame.
Pure tests cover controller detection/fallback, palm transforms, camera/seat
matching, body yaw, foot planning, room-scale accounting and weapon identity.

Validation: optimized Win32 DLL and launcher, all 43 CTests passing, and embedded
launcher payload verification. Earlier cumulative GOG/Quest feedback accepted
body rotation/leg motion, palm placement, occlusion and improved interactions.
On-foot jitter was still under investigation; this is not a claim that every
pacing issue is solved. The 1.0.4 extraction has no new headset acceptance.

Next runtime checks: saved-house strafing and physical turns, medkit/door use,
weapon cycling/F1 alignment, map/GPS/phone depth and overlays, and vehicle entry/
exit. Steam and non-Quest regression coverage remains offline only. The separate
panel and optional-motion PRs are not prerequisites, but they touch some shared
functions and may require normal conflict resolution if merged in another order.
