# Run 04 — native startup crash confirmed

**Yes: the modded game itself still crashes during main-menu construction.**
One launch, no retry. The corrected launcher retained ownership and acknowledged
the private xr-sim bootstrap. The game recorded a native read access violation
and terminated itself; no forced stop. Original files and all five saves/inventory
were restored and independently hash-verified. No FarCry2 process remains.

Launcher source: `cb65e943b7ada8fe14960b5cdccf3562c50ad4fd`, branch
`codex/tracking-arms-physical-steering`. Unchanged debug DLL SHA-256:
`bd09ac7a2665533170c244e6301de84fd5817ca42f50840caab94f8598b7fef0`,
built from `08480a82ed60b4989af982440d8172aba858bab1`. No implementation/build
change this run; nothing pushed or packaged.

## Discriminator and observations

Question: does a native startup crash remain when supervision is working?
Same exact image/DLL/INI/profile/800x800 virtual headset as preceding attempts;
changed variable is the corrected identity/shared-log launcher. Prior benign
process/live-writer controls were repeated and passed. Decision: native AV plus
the owned process's actual log confirms a game failure, not a supervisor abort.

- PID **130276**, creation **05:54:46.4574635 UTC**, affinity **65535**.
- Actual debug marker and exact supported engine gate accepted; xr-sim 1.0.0
  with D3D11 binding reached ready and focused controller/input state.
- Bootstrap acknowledged **11** commands, last `input clear`, no command error.
  The immediate acknowledgement snapshot still has old poses; the later final
  state positively confirms head **1.7 m**, fixed hands at **±0.2, 1.45, -0.45 m**,
  following disabled, FOCUSED. Acknowledgement alone was not used as pose proof.
- `startup-camera-1 world=0` and `eyes=0` prove the unowned viewport took the native
  single-pass path. The callback crash still occurs: that guard is not sufficient
  to resolve startup. This does not exclude other mod/native/runtime causes.
- At **05:56:13.301 UTC** (about **86.84 seconds** after creation): read AV
  `c0000005`, preferred VA **0x10189c0a**, read address **0x00660020**.
  Registers: ECX=00660020, ESI=099bc810, EBP=099bc810, EDI=185dc180.
  First stack-word caller candidate **0x108c9086** matches the preceding crash path.
- The actual game log records `TerminateProcess(self, code=0)` after the AV.
  Exit zero is not success. Supervisor returned failure and completed restoration
  at **05:56:13.5521539 UTC**, with `Forced=false` and `ProcessStopped=true`.

## Bounded static interpretation

Read-only pefile/Capstone against the exact engine hash confirms the fault reads
`[ECX]`; ECX was loaded from receiver+0x48 and checked only for zero. The earlier
run faulted two bytes later at 0x10189c0c. A direct call ending at 0x108c9086 targets
this helper. Existing caller inspection identifies main-menu construction.
This supports an invalid stored callback/reference, **not** a proved native type,
container bound, ownership cause or safe null-guard patch. The detailed receiver
probe only covered the earlier instruction, so this run retains registers but
not the container descriptor. No guessed binary patch was introduced.

The startup-crash observation is **VALID / CONFIRM**, baseline **FAIL**. This is
not a clean native-vs-mod causal A/B. The separately configured arm experiment
is still **INVALID / NO_DATA**: no declared-save load, on-foot owner, trace,
palette or eye capture. No rig/GPU/HMD acceptance follows.

Raw evidence, used scripts and originals remain ignored/local under
`out/native-scenario-20261026-04/`. Summary and checker/restoration controls are
under `docs/evidence/native-scenario-20261026-04/`. The local research receipt
records the startup result separately from arm acceptance; no fleet submission.

Next discriminator: inspect native menu entry/container creation and callback
ownership, including the same helper's two possible fault instructions. Keep
unknown pointers fail-closed; do not suppress the AV or guess bone/IK fixes for
this startup fault. Another game launch needs separate authorisation.
