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

The dominant wrist uses a mirrored right-hand calibration in the native left
bone basis; it does not reuse the original weapon's support-hand orientation.
The gun's placement is reflected about its native muzzle center plane and the
controller grip, with a proper rotation that preserves mesh winding. The right
wrist uses the mirrored original support orientation at the foregrip target,
and IK retains each anatomical shoulder. When released, the right support hand
uses a right-wrist calibration. Map/radio/phone and other non-weapon arm poses
keep their native anatomical controller layout rather than crossing the arms.
This conversion is local to the arm solve; it does not change action mappings,
raw XR authority stamps or the items' native attachments. Item poses cannot
publish a weapon firing ray or inherit a two-hand weapon grip latch.
The weapon is attached to the left-hand bone with the corresponding offset.
Character meshes and finger shapes are not mirrored; free-hand cosmetic cleanup
is outside this step. The native
attachment adapter is gated to the exact Steam image listed in BODY_YAW.md.
Holster zone geometry is unchanged
and should be tested/moved separately after weapon handling is accepted.

Build and run CTest as in [BODY_YAW.md](BODY_YAW.md). `hand_roles` checks pose,
button, haptic-index and authority selection in both modes. On Windows,
`weapon_controls` executes the real gamepad mapper with stubbed XR/engine
services, without a game or input injection. It checks both modes for fire vs.
reload, missing weapon tracking, physical stick locations, pause, sights and
weapon-side haptics. `hand_attachment` checks conversion across 3,367 grip poses.
`left_grip` checks mirrored palm/handle contact, muzzle-center and forward
invariants, two-hand rotation order, close-range ray parallax and spread
rotation through full and nearly antiparallel turns.
These CPU checks do not prove rendered gun placement or comfort.

Left-hand aiming uses the evaluated native muzzle origin and forward (+Y of
the muzzle transform). The slot is observed from the native muzzle/effects
strategy in `0x10110EC0` (+0x40 context, +0x54 slot), qualified against the
current weapon's graphic owner, skeleton and bone hash. **The inventory weapon
component's +0x54 is not that slot.** No resource pointer at +0x40 is treated as
a skeleton. Native shot spread is rotated from the camera basis onto the
barrel; the reticle/scope reference ray is derived from the same world muzzle.
The close-range aim query starts 1 cm from the muzzle rather than skipping
the first half metre. Non-firearm machete attachment retains its previous
calibration when no muzzle exists; its full visual/melee acceptance is pending.

The native runner's `-LeftWeapon` option enables a visual-only test fixture:
physical left/right controller targets are generated for rendering, but never
enter the XR input snapshot or receive controller action authority. Live VR
buttons are excluded from that owned test, while native action-map gates and
keyboard menu loading remain active. After a run
with `-Enabled 1 -GrassFix -LeftWeapon`, run:

```powershell
python mod/tests/check_left_weapon_native.py <run directory>
python mod/tests/test_left_weapon_native_rejections.py <run directory>
```

This checks actual Steam left/right bone readback against the weapon and
reach-limited support targets and mirrored dominant-wrist orientation, plus
attachment identity and gun world alignment after both eyes render. It also
compares the published firing ray and reticle/scope reference ray with the
actual rendered muzzle and barrel. Raw XR tracking/source stamps remain physical; the
visual fixture and observation adapters are excluded from production.

The initial controller-routing prototype loaded Continue, and physical testing
confirmed left-controller weapon movement and right-controller support grip.
That test also exposed swapped dialog buttons and crossed arms. A subsequent
test confirmed the dialog fix but reported the gun outside the left palm and
misses at close range. Reload was subsequently confirmed working, as was the
physical X button. That run reported a rightward SMG angle, crossed map/radio
arms and an offset right support hand. The current candidate corrects the item
arm layout and support-wrist orientation; the SMG angle is still unconfirmed
against original right-handed calibration. It requires a new physical test:

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
6. Open the map and radio, move each controller independently, then return to
   the gun. Check for crossed arms, correctly placed items and restored left
   weapon holding. Check the SMG/machine-gun right support wrist while gripping
   and after release. The runtime keyboard hand toggle is not implemented yet.

Do not infer acceptance of vehicles, map/phone/monocular, throwing, climbing or
holsters from the on-foot weapon test. No changes from this branch belong in
the separate upstream body-yaw PR.
