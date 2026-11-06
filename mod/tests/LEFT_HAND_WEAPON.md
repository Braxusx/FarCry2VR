# Left weapon-hand prototype

Set `LeftHandedWeapon=1` in `[VR]` in `fc2vr.ini`, then restart. Default 0
retains right-handed controls. This first step selects the physical left
controller for weapon pose, aiming, firing and weapon haptics, and the physical
right controller for support grip and reload. Face buttons keep their original
physical mappings, including dialog confirmation. The left stick still moves/sprints and
the right stick still turns/crouches/heals. Pause stays on the menu button.

Raw XR poses and authority stamps stay indexed by physical hand. Per-hand
freshness, rearming and generation checks select the configured physical hand
at each use, including deferred interaction and throw checks. Dropping weapon
tracking cannot borrow the support controller's tracking.

The native left wrist is moved to the calibrated weapon target, the native
right wrist follows the support target, and IK retains each arm's anatomical
shoulder. The weapon is attached to the left-hand bone using a converted offset
that preserves its calibrated world pose. Character mesh and finger shapes are
not mirrored; cosmetic free-hand pose cleanup is outside this step. The native
attachment adapter is gated to the exact Steam image listed in BODY_YAW.md.
Holster zone geometry is unchanged
and should be tested/moved separately after weapon handling is accepted.

Build and run CTest as in [BODY_YAW.md](BODY_YAW.md). `hand_roles` checks pose,
button, haptic-index and authority selection in both modes. On Windows,
`weapon_controls` executes the real gamepad mapper with stubbed XR/engine
services, without a game or input injection. It checks both modes for fire vs.
reload, missing weapon tracking, physical stick locations, pause, sights and
weapon-side haptics. `hand_attachment` checks conversion across 3,367 grip poses.
These CPU checks do not prove rendered gun placement or comfort.

The native runner's `-LeftWeapon` option enables a visual-only test fixture:
physical left/right controller targets are generated for rendering, but never
enter the XR input snapshot or receive controller action authority. After a run
with `-Enabled 1 -GrassFix -LeftWeapon`, run:

```powershell
python mod/tests/check_left_weapon_native.py <run directory>
python mod/tests/test_left_weapon_native_rejections.py <run directory>
```

This checks actual Steam left/right bone readback against the weapon and
reach-limited support targets, plus attachment identity and gun world alignment
after both eyes render. Raw XR tracking/source stamps remain physical; the
visual fixture and observation adapters are excluded from production.

The initial controller-routing prototype loaded Continue, and physical testing
confirmed left-controller weapon movement and right-controller support grip.
That test also exposed swapped dialog buttons and crossed arms; the updated
candidate corrects those in code and requires a new physical test:

1. Hold/move/rotate the left controller; verify the weapon follows it and aiming
   remains aligned. Move the right controller independently.
2. Left trigger fires; right trigger reloads without firing.
3. For a two-handed weapon, bring the right hand to the foregrip and squeeze
   its grip; check support, sights and release. Then try a pistol.
4. Confirm left-stick movement, right-stick turning and physical body yaw.
   Check that dialog confirmation still uses the original physical button,
   the character's left hand holds the gun, and the arms do not cross.
5. If available, compare SMG and RPG placement. Existing free-hand pose quirks
   observed in right-handed play are not acceptance blockers for this step.

Do not infer acceptance of vehicles, map/phone/monocular, throwing, climbing or
holsters from the on-foot weapon test. No changes from this branch belong in
the separate upstream body-yaw PR.
