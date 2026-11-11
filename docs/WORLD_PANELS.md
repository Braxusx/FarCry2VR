# World-anchored screens

`WorldPanels=1` opens HUD, pause-menu and movie/loading panels upright in OpenXR
LOCAL space. Head movement does not drag them around. Changing panel mode,
recentering, or replacing the reference space chooses a fresh anchor. Existing
`MenuDistance`, `MenuWidth` and `UiHeadY` control placement. `WorldPanels=0`
retains the legacy HUD/menu placement.

The HUD uses the desktop UI layout aspect ratio rather than the eye texture's
shape; widescreen menus no longer look squeezed into a portrait panel. Native
cinematic/lying-down action maps use the flat presentation and release VR hand
overrides instead of presenting the forced camera movement stereoscopically.

Validation on the 1.0.4 base: optimized Win32 DLL build and all 24 CTests pass.
The new helper test checks stationary anchors, mode changes, recenter/reference
replacement, invalid tracking and logical versus texture aspect ratio. Earlier
cumulative GOG/Quest testing accepted fixed menus and their proportions. This
isolated 1.0.4 extraction still needs headset checks of startup movies, pause
menus, the bed cinematic, recentering, and the legacy switch. No performance or
universal controller-compatibility claim is made.
