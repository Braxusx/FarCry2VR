# Native startup guard and headless verification

2026-10-26. Local contribution branch only; no push, release or permanent deployment.

## Result

The native startup callback crash is prevented in the tested configuration by
`menu-record-guard-2`. A separate native exception on **window-close** remains
unresolved. The overall build is **not** cleared as healthy or release-ready.

Latest run 12 loaded the normal, non-scenario DLL, acknowledged the private x86
xr-sim fixture, installed all three guard hooks after four unique signature
checks, and survived its 150-second startup window plus 120-second quit grace.
There was no menu-driven input during that run. At `08:35:54.5871513Z` the
supervisor called `CloseMainWindow`; at `08:35:54.603Z` the game logged read AV
`c0000005` at preferred VA `0x10ad4095`, address `0x0000000c`, then self-terminated
with code 0. The final-log check correctly makes this a failed run, despite
`StartupSurvived=true`, `Forced=false`, and exit code 0.

Far Cry 2 is stopped. Original proxy, INI, profile, logs and all five saves plus
their exact inventory were independently hash-checked after restoration. The
tested DLL is **not left installed**. A follow-up preflight found Swat4X PID
94272, created `08:36:12.577883Z`, after Far Cry 2 cleanup finished at
`08:35:55.1093632Z`. The follow-up was refused **before another launch**; no SWAT
process was touched. Menu-driven quit remains unmeasured.

Retained latest evidence: [summary](evidence/menu-record-guard-2/summary.json),
[native log](evidence/menu-record-guard-2/run-12-test-fc2vr.log),
[cleanup](evidence/menu-record-guard-2/run-12-cleanup.json),
[61 passing CTest groups](evidence/menu-record-guard-2/ctest.log).
The summary retains file/save hashes, source hashes, binary identity and the
post-test process observation. Test coverage does not establish visual quality.

## Confirmed startup defect and bounded repair

Exact supported Steam/Definitive `Dunia.dll`: 20,183,176 bytes, SHA-256
`6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765`.
The x86 executable SHA-256 is
`b5e081fe22c09618eeee68e0e470ce29c7e539100faf849c926b77a4e2fb4db8`.
Addresses below are preferred-image VAs, not independently portable offsets.

The menu object embeds a container at `+0x10c`; its descriptor pointer is at
`+0x118`. The native getter at `0x1018a390` computes `base + index * 84` without
bounds checking. Its companion count getter establishes descriptor `+8` as
constructed count. Descriptor `+0` is the measured `0x10e289fc` vtable;
constructed 84-byte records have vtable `0x10e25054` and callback at `+0x48`.

The optional menu-animation paths request **index 4 (the fifth record)** even
when the constructed count is four. Runtime observations distinguish constructed
records 0..3 from spare, unconstructed bytes at index 4. Without VR, the spare
callback happened to be zero. With VR it contained unrelated pointer-looking
data. The callback setter at `0x10189c00` then tried to release that nonexistent
old callback. This accounts for startup faults at `0x10189c0a`,
`0x10189c0c`, or a subsequent invalid callback dispatch from the same caller.

These are **not the visible menu entry indices**. The native list separately
creates six visible labels. No specific missing asset, multiplayer removal,
DRM fault, arm index or wrist deformation is established by this finding.
No assets or multiplayer paths are modified.

`mod/src/main.cpp` now guards three owners of the optional record:

- Init tail `0x108c904f`: branch to its native epilogue when absent, before
  allocating/registering the optional callback. No dummy record or leaked callback.
- Reset `0x108c7fa0`: retain the original only when the extra record is constructed.
- Tick `0x108c82c0`: retain the original when present; otherwise call its proved
  standard parent/list update `0x10108c10` exactly once, skipping optional animation.

The optional event callback's other fifth-record accesses are reached through
the callback allocated by the guarded init tail; absence prevents its registration.
The check uses safe reads, descriptor identity, constructed count 5..64 and record
identity. Readable spare memory alone is never accepted. It does not patch the
callback destructor/getter globally or swallow native exceptions.

The installer accepts only the exact Steam image and prevalidates a cohort of
unique executable-section contexts (31/13/18 bytes, plus 9-byte parent context),
expected resolved locations and native epilogue. Short init/tick prologues alone
are nonunique and are deliberately insufficient. A mismatched cohort refuses
patching and disables VR. Unknown images retain the pre-existing image refusal.

`[VR] MenuRecordGuard=1` is the default bounds-safety repair, with proof logs;
`MenuRecordGuard=0` is a diagnostic revert and reproduced the startup crash.
Do not leave it disabled for normal use. This is not a new camera/input experiment.
Optional raw menu observations require a debug build **and** process opt-in
`FC2VR_MENU_DIAG=1`; the normal build excludes that payload.

## Discriminating runs

Raw run directories are retained locally under `out/startup-isolation-20261026-*`.
Earlier immutable cleanup records are not rewritten to hide recovery failures.

| Run | Changed variable / artifact | Observed result |
| --- | --- | --- |
| 05 | Earlier debug DLL, VR disabled | 140-second survival; original cleanup failed; subsequently manually restored and checked. |
| 06 | VR disabled, explicit read-only getter observer | 180-second survival; constructed count 4, spare fifth callback zero; restoration passed. |
| 07 | VR enabled, observer, no guard | Count 4; spare fifth callback contains unrelated pointers; native startup AV and self-termination; restoration passed. |
| 08 | Guard enabled, debug guard-1, no menu observer | 180-second survival; loading/on-foot action map, ready pawn/rig/weapon ownership and thousands of stereo/arm updates observed. Cleanup copy hit late DLL sharing; manual recovery restored originals. Specific save load and visual quality not proved. |
| 09 | **Same DLL as 08**, only `MenuRecordGuard=0` | Startup AV at `0x10189c0a`, same init caller `0x108c9086`; restoration passed. |
| 10 | Final normal guard-2 DLL | 240-second startup survival, then window-close AV at `0x10ad4095`. Older cleanup reported success without inspecting post-close log; superseded by the final-log check. |
| 11 | Same final normal DLL | Interrupted following user Escape/Computer Use stop before XR acknowledgement; bounded cleanup force-stopped the owned process, restored originals. Invalid startup acceptance. |
| 12 | Same final normal DLL; hardened final-log checker | Startup and XR remained active; no world/stereo gameplay acceptance; **shutdown FAIL** recorded correctly. Exact originals restored, no forced stop. |

Guard-1 debug used in 08/09:
`283d69676e4c66a55b2abd4c9f1f6e7d04ec76fff501649cb95e0aa670ddebfb`.
Final guard-2 normal tested in 10..12:
`407c774f32fd5b91ff72dc5572a75987e3970168a6ee4d762a85b0208d2e2b70`.
Final guard-2 debug built, not launched:
`3bb3377ae0df448e031f4ee62a92d9183b89743026bf94726b163467fdedd5ad`.

Both final DLL variants build as PE32/i386, export `DirectInput8Create`, and pass
scenario-payload inclusion/exclusion inspection. Artifacts/build logs are in
`build/menu-record-guard-2-{production,debug}/`. This session reran all 61 offline
groups after launcher changes; no game launch is part of that suite.

## Launcher contract and next discriminator

`scripts/Test-NativeScenario.ps1 -StartupOnly -ProductionProxy` clears synthetic
scenario arming, verifies the loaded normal marker and observes bounded survival.
`-MenuQuitGraceSeconds 120` permits an external state-observing UI driver to quit
through the native menu after that window. It does **not** perform menu input.
If the game remains, identity-bound `CloseMainWindow` is attempted, then a bounded
forced-stop fallback. Cleanup records close method/time and post-close native-log
failure independently from survival, restoration and process exit. Process wrapper
disposal and bounded restoration-copy retries address earlier handle/sharing failures.

Next, when no fleet game is running: use this same normal DLL and fixture, actually
observe/deliver the native Quit action during grace, and compare its final log with
the repeated window-close fault. If both fault, obtain the exact shutdown receiver/
caller contract before making a new native patch. Do not suppress the exception or
declare shutdown fixed because the process stopped. Root cause of this second
fault, actual arm scenario captures, weapon swaps/tracking-loss sweeps, GPU/palette
ownership and HMD acceptance remain open.
