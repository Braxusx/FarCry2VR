# Input and pose authority — offline checkpoint

Date: 2026-10-23. Branch: `codex/tracking-arms-physical-steering`.
Parent: `371c2aa` (shared-camera-1). Startup stamp: `contribution input-authority-1`.
This is the first R4 safety slice, not completion of PR 2 or runtime acceptance.

## Implemented boundary

`common/pose_authority.h` owns portable freshness, evidence, visual-cache,
neutral-rearm, sample-cursor, numeric-sanity and pointer-owner policy. Production
adapters use it directly; tests do not copy its implementation.

- Input publications carry an original monotonic receipt, sequence, focus/sync
  state, combined lifecycle generation and per-hand tracking epochs. XR predicted
  time is retained separately and is not an elapsed-time clock.
- Raw pose reads expire after 100 ms. Both the original source and latest input
  must be fresh; rendering or reapplying a skeleton does not renew a pose receipt.
- Physical authority requires active, position/orientation-valid and tracked
  evidence, finite positions/orientations, a tracked head and neutral rearming.
  A held cosmetic pose never becomes physical evidence. Analog input is sanitized.
- Focus/sync loss, publication stalls, recenter, session teardown and LOCAL-space
  changes revoke authority. Reference changes are queued by their XR change time;
  physical control is suppressed while pending. This resets/recenters room state;
  it does not preserve/rebase old room anchors.
- Publication invalidation revokes the shared snapshot under its lock before
  publishing neutral input. Failed/partially tracked view location also revokes
  spatial authority without pretending ordinary fresh buttons are stale.
- Startup/focus/sync/gap/recenter recovery requires neutral buttons, triggers,
  grips and sticks. After pose-only loss, physical control requires tracked poses
  and released trigger/grip; locomotion need not stop to rearm that hand.
- XInput mapping advances once per publication, not once per game poll. Loss
  cancels gesture history and pending requests directly, without synthesizing a
  release action. Native real-pad fallback remains available.
- Movement, reload and menu buttons can remain usable during position loss.
  On-foot pose-guided fire, throws and hand contacts cannot use held visual poses.
  Native vehicle acceleration, mounted-turret controls and menu input remain
  independent of hand tracking. Positive haptics require fresh input.
- Arm frames distinguish cosmetic validity from physical authority. Aim and scope
  rays retain original source stamps and weapon-instance ownership; smoothing and
  support latches reset on relevant context/owner changes. Contacts and cached NPC
  drive records reject stale source evidence; drive records are copied under lock
  and native writes occur outside it.
- A released throw can become an accepted native-animation intent, distinct from
  a raw live pose. It retains its original context and existing three-second
  deadline, requires healthy current input and matching pawn-data context, and is
  cancelled by tracking/focus/space context changes. It does not require a live
  raw pose to survive the native animation delay. Native request-ticket and
  throwable-identity acknowledgement are not implemented by this change.
- Eye positions must be valid and finite before recenter, persistent height,
  scope or 6DoF use. Invalid positions take the existing native flat fallback;
  they cannot poison the persistent recenter origin with NaN values.

## Offline evidence

MSVC x86 Release (`/W4 /WX`) passes all 19 CTest groups: 17 numeric groups with
31,225 explicit checks, plus two source-wiring guards. Of these, six new numeric
groups exercise 477 authority checks: freshness 118, cache 106, rearm 210,
cursor 6, safety 28 and finite values 9. The new groups also pass GCC i686 with
`-O2 -Wall -Wextra -Werror`; the unchanged earlier numeric groups previously
passed that compiler. Source-wiring guards are not execution or concurrency proof.

Two deliberate mutations were rebuilt against the final production header:
removing receipt expiry and bypassing neutral rearm. The corresponding freshness
and rearm test runs each fail with exit 1, as expected. Mutant sources/binaries
remain ignored test artifacts and are not part of the DLL.

Development DLL: `build/input-authority-1/dinput8.dll`, 1,237,131 bytes.
SHA-256: `f3211e3a51106a7e644235adbf78b179f41239cdcbec9afa92f067eb40af659d`.
GCC 16.2.0 i686 builds the full proxy with the existing build source/library list.
PE32/i386, undecorated `DirectInput8Create` and Windows-only imports were checked.
The same 32 existing warning diagnostics remain; no additional warnings.
Only process-local compiler PATH setup was used. The documented llvm-mingw route
is still unverified because that compiler is not installed. The launcher and DLSS
helper were not rebuilt. Logs and controls are ignored under
`build/input-authority-1/`; CTest output is also recorded there.

No game launch, deployment, packaging, remote push or PR was performed. These
results establish policy behavior and compilation, not native or headset acceptance.

## Remaining gates

R4 remains partial: full session/pawn/model/palette/cache ownership and native
target lifetime/ABA protection, NPC animation/driver leases, reload/attachment
ownership and native throw acknowledgements are not complete. Weapon-pointer
checks do not prove model or bone-cache identity. Visual holding still uses native
skeleton presentation; this is not an isolated render-only palette architecture.
Inherited input-thread assumptions, haptic teardown and native job concurrency
need runtime validation. R9 unknown-image refusal is still open.

R1's absolute shared-frame math is implemented, but native hierarchy scheduling
may consume a previous publication. Frame-use counters need a controlled run;
walking jank is not accepted as fixed. R2 fractional wrist twist, R3 animated
support sockets, R6/R7 torso/rig robustness, R8 spread transport and R10 broader
reload/attachment identity remain open. Physical steering, full manual reloads
and optional DQS are not implemented by this checkpoint.

Next offline priorities: exact-image/native-access gating and bounded wrist-twist
regressions. Next authorized runtime run: verify publication consumption, input
loss/rearm and native-pad fallback, weapon swaps/support/scope, interrupted throws,
focus/recenter/reference changes, then headset walking/head-only/hand-only motion.
Close only the owned Far Cry 2 process when that run finishes.

Rollback this focused commit as a whole to parent `371c2aa`. No deployed files,
runtime configuration or shared machine settings require restoration.
