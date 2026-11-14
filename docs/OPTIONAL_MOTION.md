# Optional arm-swing sprint and motion steering

All gestures are opt-in. `ArmSwingSprint=0`, `MotionSteering=0`, and
`DrivingHandbrakeOnA=0` preserve the original controls. The launcher exposes
these settings and updates its bindings page to match the saved layout.

- Sprint: hold the movement stick forward and pump both arms. Poses are sampled
  on the OpenXR predicted-display clock, not the coarse host timeout clock.
  Tracking loss, recentering, reload/climb activity, aiming, held grips,
  gadgets, vehicles and non-gameplay contexts cancel the gesture. Stick-click
  sprint remains available. Stamina and native sprint eligibility remain native.
- Steering: acquire the virtual wheel with both grips ahead of the body. Turn
  the hands; either stick deliberately moved beyond 0.2 overrides the motion.
  Native arm/finger contact stays on the wheel while held. Releasing a hand
  restores its ordinary VR pose. `SteeringFullLockDegrees` defaults to 90 degrees
  per direction and can range from 30 to 180.
- With motion steering, grips are reserved for the wheel. By default A changes
  seat and left-stick click holds the handbrake. `DrivingHandbrakeOnA=1` swaps
  those two actions, independently of whether motion steering is enabled.
- `MotionSteeringDirect=1` reduces native catch-up only for owned GOG motion
  input, retaining speed-dependent angle limits. Unsupported editions or byte
  mismatches retain the native response. Turret motion aiming remains upstream.

## Native adapter boundaries

GOG adapters verify exact instruction windows before patching. Axis bypass is
limited to the exact synthesized pad packet, input owner and current pose lease.
The sprint adapter only relaxes body-facing direction checks for a fresh,
head-directed gesture; the other native eligibility branches still execute.
The steering resolver follows the local pawn to its mounted vehicle/physics/
steering component. A wheel-like skeleton alone never establishes ownership.
Invalid ownership, stale input, changed vehicle, sticks, tracking loss and
context changes release overrides. Native saturation and input events remain.

`MotionInputTrace=1` enables bounded text diagnostics; no captures are required.
The earlier cumulative GOG/Quest build was accepted for sprint, steering, held
wheel hands and this layout, with a small residual steering delay. This isolated
1.0.4 extraction is not headset-accepted. Steam gesture behavior and compatibility
with the separate left-handed PR need runtime testing.

## Validation

Optimized Win32 DLL and launcher build. All 33 CTests pass: gesture replay,
tracking/generation resets, driving layouts, byte-exact held-hand preservation,
executed x86 adapter stubs, return/register behavior, native owner rejection and
installation guards. The native instruction-window test can also read a local
GOG Dunia DLL without loading or executing it. Tests do not establish headset
latency, comfort or compatibility with every vehicle.
