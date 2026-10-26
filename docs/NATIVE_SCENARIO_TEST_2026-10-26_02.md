# Second authorised native attempt — 2026-10-26

**Result: INVALID — supervisor stale-log race, not a native crash reproduction.**
Exactly one game launch; no retry. The game is stopped. Original proxy, INI,
graphics profile, current/previous logs and all five saves/inventory independently
match pre-run hashes. Nothing pushed, packaged or sent to fleet intake.

Source: `08480a82ed60b4989af982440d8172aba858bab1` on
`codex/tracking-arms-physical-steering`. Debug DLL SHA-256:
`bd09ac7a2665533170c244e6301de84fd5817ca42f50840caab94f8598b7fef0`
(1,378,215 bytes). Production build:
`84114ac2301e74fe2a177b900ee6b8db2b45080bb1f3795312a20ae77969131a`
(1,311,743 bytes). Both PE32/i386 builds verify the proxy export and scenario
payload inclusion/exclusion, with 31 existing warnings each.

## What happened

The new suspended-process launcher returned the actual game PID **99500**, exact
executable path, creation ticks **639285884787762599** and affinity **65535**.
Created 2026-10-26 **05:14:38.7762599 UTC**; cleanup finished **05:14:51.5461238 UTC**.
The 240-second watchdog and cleanup loop were reachable, unlike the preceding
blocking-shell attempt. However, the first telemetry read consumed the installed
previous session's `fc2vr.log`, before the new DLL opened its log, and reported
`Wrong loaded mod marker`. This check omitted a fresh-generation guard.

The actual archived new log then shows the **correct**
`startup-camera-1/scenario-runner-2, scenarioTesting=1` marker, exact Steam engine
acceptance, 16-CPU affinity, configured arm sweep and hook installation. Thus the
supervisor's marker failure was false. No alternate DLL load was demonstrated.

The supervisor requested graceful closure, waited 12 seconds, then stopped only
its path/creation-time-matched PID. The graceful request did not stop it within
that interval; window availability/acknowledgement was not recorded.
`Forced=true` is a failed-cleanup-quality flag, not a passing test.
No further game was launched.

## Evidence limits

This attempt ended during early startup, well before the preceding attempt's
~94-second native main-menu failure. No XR runtime state, bootstrap acknowledgement,
first world-camera-policy decision, scene/save load, scenario trace, palette or
eye capture was observed. The absence of an exception in this short window says
nothing about resolution of the original callback crash. Native startup baseline
is **NOT_MEASURED**, arm result **AMBIGUOUS**, experiment **INVALID**. No HMD or
visual acceptance.

The startup guard and debug register/receiver observations are implemented, but
not runtime-accepted. The previous callback crash's root cause remains unresolved;
no guessed native pointer guard or binary patch was added.

## Offline correction after the run

The used script/module are preserved unchanged under ignored
`out/native-scenario-20261026-02/`. Current tracked supervisor additionally moves
both backed-up prior logs into its private run directory before launching,
requires a fresh file generation, and centralises log verdicts. Regression controls
reproduce stale wrong-build/runtime/AV input, accept a fresh correct marker, and
reject fresh wrong marker/runtime/AV. The Python-child affinity/nonblocking/PID-
reuse controls still pass; they are not another game run or XR control.

The strict checker returns **2 / NO_DATA** on the actual empty scenario directory.
Review bundle generation returns **1 / INVALID: NO_DATA** and creates no output.
All **60** MSVC x86 CTest groups pass after the correction, including 16 receipt
and 13 review test methods plus the launcher controls. No-data is not PASS.

Raw logs, original files, saves, used scripts and backup metadata remain local and
ignored. Structured non-game-content summary: `evidence/native-scenario-20261026-02/`.
Standalone local receipt: `../receipts/20261026-native-scenario-02.json`.

## Next discriminator

Another native launch requires renewed authorisation. Use the corrected fresh-log
supervisor and post-start simulator acknowledgement; first establish a surviving
menu. If the same callback fails, the debug-only bounded registers/receiver dump
can discriminate the indexed native container from a corrupt callback rather
than infer it from a filtered stack. Only then load a disposable on-foot scene
and collect the 22-pair arm sweep. The one-run budget is exhausted here.
