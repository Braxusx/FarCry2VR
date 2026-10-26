# Startup camera and supervised native test — local contribution

Branch: `codex/tracking-arms-physical-steering`. No release/push/PR.

The preceding native attempt is preserved in `NATIVE_SCENARIO_TEST_2026-10-26.md`.
Its native callback crash is not a proved arm-rig failure. Exact-image static
inspection identifies a main-menu construction caller (including the
`MAIN_MENU_QUIT_GAME` string), index 4 of an 84-byte-record container, and the
replacement helper that dereferences an existing callback at receiver+0x48.
These are byte-level observations, not a complete native type or root-cause proof.
Ghidra's current GUI belongs to SS2; it was not switched or repurposed.

## Implemented safety changes

- `startup-camera-1`: require an observed player plus non-loading/non-menu
  controls before camera posing, scope evaluation, arm publication or stereo
  viewport replay. XR tracking alone previously enabled stereo at a zero/default
  startup camera. The existing native single-pass/backbuffer-to-XR-screen path
  remains available. No weapon requirement: unarmed/vehicle states are not rejected
  merely for lacking a gun. This is not an authored-cutscene policy.
- Debug-only `crash-context-1` retains registers and bounded ReadProcessMemory
  observations for the exact failing instruction/caller. No arbitrary callback
  null guard, exception swallowing, guessed native ABI or executable patch.
- `scripts/Test-NativeScenario.ps1` is plan-only by default and needs explicit
  paths, exact image identities and a new run directory to execute. It launches
  **once** through CreateProcessW suspended, applies a low-16-CPU affinity before
  resume, uses the returned PID plus path/creation time, and inherits no stdio
  handles. There is no shell-helper wait or name-based PID selection.
- Only after a fresh matching xr-sim PID/state appears does it send the fixture
  bootstrap and require its exact sequence/last-command/error acknowledgement.
  Runtime/layer selection is process-scoped. A bounded watchdog, operator stop,
  native-AV detection, graceful-close/owned-PID fallback and hash restoration
  remain active independently of scenario progress. Zero exit is not success.
- Original DLL/INI/profile/logs and existing saves are backed up and restored.
  No launch retries. Raw proprietary evidence and backups stay ignored in `out/`
  and `build/`, never in Git. A terminal marker still needs the strict checker.

## Offline checks

`mod/tests/test_native_launcher.ps1` parses both scripts, checks plan-only and
stale-directory refusal, launches only a benign Python child, verifies prompt
return/affinity and rejects a deliberately mismatched creation time without
touching that child. Pure policy tests cover all eight input combinations and
revocation after gameplay. Source wiring retains all original pose prerequisites.
Build-only `mod/build-local.ps1 [-ScenarioTesting]` now uses fresh
`build/startup-camera-1-{debug,production}/` paths; earlier run binaries are retained.

## One authorized next experiment

Question: with supervised/acknowledged virtual XR and an unposed single-pass
front end, does the same native startup callback crash recur?

Hypothesis: early replay/posing of the unowned startup viewport is unsafe.
Control: exact same game image, declared save, 800x800 virtual headset and INI/
graphics fixture as the previous attempt; offline checker controls remain the
instrument positive/negative controls. Variables: startup camera policy and
corrected launcher/bootstrap (therefore **not** a clean single-variable causal A/B).
Decision: any native exception refutes crash-resolution acceptance; a surviving
menu is startup evidence only, and arm acceptance still requires the observed
on-foot owner plus all correlated scenario records/checker. If it crashes, use
the added registers/receiver evidence to select the next static discriminator;
do not spend another launch without authorization. Always close the owned game.

Native result: [run 02](NATIVE_SCENARIO_TEST_2026-10-26_02.md) was invalidated by
a stale installed-log race and stopped during early startup. The corrected
supervisor now moves prior logs into its private run directory before launch,
checks file freshness, and has stale/fresh-marker/runtime/AV regression controls.
Those corrections were tested offline only. No second launch was taken.
