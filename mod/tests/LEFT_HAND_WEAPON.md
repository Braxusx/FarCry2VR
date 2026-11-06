# Left weapon-hand prototype

Set `LeftHandedWeapon=1` in `[VR]` in `fc2vr.ini`, then restart. Default 0
retains right-handed controls. This first step selects the physical left
controller for weapon pose, aiming, firing and weapon haptics, and the physical
right controller for support grip and reload. Face-button action roles also
swap (left X jumps; right A interacts). The left stick still moves/sprints and
the right stick still turns/crouches/heals. Pause stays on the menu button.

Raw XR poses and authority stamps stay indexed by physical hand. Per-hand
freshness, rearming and generation checks select the configured physical hand
at each use, including deferred interaction and throw checks. Dropping weapon
tracking cannot borrow the support controller's tracking.

This is an initial gameplay prototype. It still drives the engine's existing
right-handed weapon/arm rig from the selected controller; it does not mirror
the character mesh or recreate anatomical left-handed finger poses. Cosmetic
free-hand pose cleanup is outside this step. Holster zone geometry is unchanged
and should be tested/moved separately after weapon handling is accepted.

Build and run CTest as in [BODY_YAW.md](BODY_YAW.md). `hand_roles` checks pose,
button, haptic-index and authority selection in both modes. On Windows,
`weapon_controls` executes the real gamepad mapper with stubbed XR/engine
services, without a game or input injection. It checks both modes for fire vs.
reload, missing weapon tracking, physical stick locations, pause, sights and
weapon-side haptics. Neither test proves rendered gun placement or comfort.

The production DLL built successfully and loaded Continue automatically with
the option enabled. Physical acceptance is pending. First test:

1. Hold/move/rotate the left controller; verify the weapon follows it and aiming
   remains aligned. Move the right controller independently.
2. Left trigger fires; right trigger reloads without firing.
3. For a two-handed weapon, bring the right hand to the foregrip and squeeze
   its grip; check support, sights and release. Then try a pistol.
4. Confirm left-stick movement, right-stick turning and physical body yaw.
5. If available, compare SMG and RPG placement. Existing free-hand pose quirks
   observed in right-handed play are not acceptance blockers for this step.

Do not infer acceptance of vehicles, map/phone/monocular, throwing, climbing or
holsters from the on-foot weapon test. No changes from this branch belong in
the separate upstream body-yaw PR.
