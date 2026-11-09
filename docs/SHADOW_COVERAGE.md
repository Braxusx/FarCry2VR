# Shadow coverage

The launcher's **Settings > Display > Shadows > Expanded shadow coverage**
checkbox lets you trade extended shadow reach for frame rate. Apply the setting
with the game closed, then start the game.

- **On (default):** preserves the mod's existing expanded sun-shadow coverage.
- **Off:** uses the game's native cascade reach. Shadows remain enabled, along
  with the VR field-of-view/aspect correction, but shadows farther away or to
  the side may disappear sooner.

The **Sharp shadow reach** slider is disabled while expanded coverage is off. Its
saved value is retained for when you turn expanded coverage back on. Shadow
resolution still applies in either mode.

For manual configuration, set `ShadowExpanded=1` (expanded) or
`ShadowExpanded=0` (native reach) in the `[VR]` section of `fc2vr.ini`.
Missing keys default to `1`, so existing installations keep their behavior.
This option requires `ShadowFix=1`; it does not turn shadows on or off in the
game's graphics settings. With `DevKeys=1`, F2 still toggles coverage for the
current session; it does not save the change to the INI.

## Performance observation and limits

In one GOG/Quest test scene outside a house, Mike reported Virtual Desktop
readings of approximately **74 FPS / 13 ms with expanded coverage**, versus
**90 FPS / 9 ms with native reach**. This was a local development build before
1.0.4, not a benchmark of this rebased contribution. It is evidence that the
trade-off can help that setup and scene, not a guaranteed gain or a claim of
90 FPS throughout the game. The 1.0.4-based build still needs headset validation.

This setting initializes the existing F2 coverage toggle. It does not change
the cascade-fit algorithm, 1.0.4 texel snapping, shadow sharing, or GPU handover.
When expanded coverage is off, its radial-fit/texel-snap path is bypassed just
as it is with the existing F2 toggle.
