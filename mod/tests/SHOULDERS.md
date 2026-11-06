# Shoulder orientation through full head turns

The previous IK projected head forward horizontally, formed right = forward
cross up, then negated right when its dot with the reference game-right vector
was negative. Beyond +/-90 degrees this exchanged shoulder origins while hands
remained on their controller targets. Physical body rotation exposed the error.

The shared shoulder-frame helper keeps the cross-product direction through
full turns. Nearly vertical or nonfinite projections request the existing
game-basis fallback. Hand targets, shoulder spacing, bone lengths and IK/twist
settings are unchanged. The correction applies independently of the body-yaw
option.

`shoulder_frame_tests` exercises four full turns, pitches from -80 to 80
degrees, angle wrapping and invalid projections (38,909 assertions). Restoring
the old hemisphere rule fails this regression. These math checks cannot verify
skin deformation. Physical headset testing accepted the correction; further
checks should turn left/right past 90 degrees, look behind and cross +/-180
while inspecting each upper-arm origin, then pitch and return forward.

See [body-yaw testing](BODY_YAW.md) for builds, automated loading, native
regressions and the limits of current coverage.
