# Native attempt 03 — launcher identity and live-file failures

**INVALID startup experiment. One launch, no relaunch. Game stopped; original
files and five saves/inventory restored and independently hash-checked.**

Used launcher: `b6dbe0a5d290222891f4570f32b5c434ffc88b22` on
`codex/tracking-arms-physical-steering`. Same existing debug DLL as run 02:
`bd09ac7a2665533170c244e6301de84fd5817ca42f50840caab94f8598b7fef0`,
built from `08480a82ed60b4989af982440d8172aba858bab1`. No C++/DLL changes this run.
Post-run launcher fixes: `65ae9eb1f11a2737a1085fc77e6a802b71f4ac7c` (offline only).

## Question and decision rule

Same image/800x800 virtual headset/INI/profile/declared save as the earlier
attempts; changed variable was the corrected stale-log supervisor. Does the
native main-menu callback crash return with valid supervision and bootstrap?
Any native exception fails startup acceptance. A surviving menu alone would not
accept an arm rig; that still needs an observed on-foot scene and correlated
captures. Maximum 240 seconds from original process creation, plus close grace.

## Actual sequence

1. The native CreateProcess/affinity helper created PID **74924**, but its immediate
   managed `Process.Path` validation threw `Created process path mismatch` before
   returning identity to the caller. The unexpected actual path value was not
   retained, so a cold loader/PEB lookup race is a **lead**, not a measured root cause.
   The initial cleanup correctly refused to restore shared game files while a
   FarCry2 process remained, but incorrectly wrote `ProcessStopped=true` from a
   null owned identity. That original receipt is preserved and must not be trusted
   as cleanup success.
2. Read-only process and CIM observations identified the one newly created game:
   exact executable path, parent **106768**, start **05:26:48.8619563 UTC**,
   ticks **639285892088619563**, affinity **65535**. No pre-existing game had been
   present. A recovery supervisor bound that existing PID; it contains no launch
   call and retained the original **05:30:48.8619563 UTC** deadline. Supervision
   had a gap; this is not a clean uninterrupted launcher validation.
3. The first recovery guard used a parsed local DateTime against UTC and refused
   before mutation. It was corrected to the independently observed exact creation
   ticks, then the same process was bound. No restart.
4. Recovery immediately encountered the second defect: `File.ReadAllText` could
   not open the live native log with its writer still open. Recovery requested
   graceful closure (`CloseMainWindow=true`), waited 12 seconds, then stopped only
   the identity-matched process. Final cleanup **05:28:21.8657191 UTC**, about
   **93 seconds** after creation. Forced cleanup is not successful completion.
5. The archived new log proves the correct debug DLL and exact engine image,
   native hook installation, D3D setup and entry into XR extension discovery.
   There is no runtime-ready/state/command acknowledgement, first camera-policy
   decision, native AV/register record, scene/save load, arm trace or eye capture.
   Cleanup cut off XR initialization, before coverage of the previous callback
   fault. No original-crash resolution or regression verdict is possible.

## Offline corrections and discriminating controls

- Capture path, PID and creation time through **QueryFullProcessImageNameW and
  GetProcessTimes on the actual CreateProcess handle, while still suspended**.
  Validate that identity before ResumeThread. Return it even if a short-lived
  child exits before PowerShell resumes. No throwing post-resume module lookup
  can lose the created identity. Owner checks inspect the managed object's bound
  native handle, not a new PID lookup or early MainModule path.
- Read log/trace with `FileShare.ReadWrite | Delete`, a capped snapshot byte count,
  and pending results for missing/rotating/exclusively locked files. The independent
  overall watchdog still runs. The same reader is wired to both live files.
- Unknown identity after an attempted launch now records `ProcessStopped=null`,
  not a false true. It does not authorise closing an arbitrary name match.
- A held-open writer fixture **reproduces the old reader failure**, while the new
  reader succeeds and observes an appended AV. Exclusive/missing/oversize controls,
  stale/fresh marker/runtime controls, eight short-lived Python children, normal
  nonblocking/affinity checks and wrong-path/creation-time refusal all pass.
  All **60 CTest groups pass**. These are non-game controls, not native acceptance.

The used scripts/logs, original failed cleanup, recovered identity/final cleanup
and full CTest log are retained in ignored `out/native-scenario-20261026-03/`.
The no-data verifier returns **2**; review builder **1**, creating no review output.
Summary/control evidence is under `docs/evidence/native-scenario-20261026-03/`;
the local research receipt retains INVALID / AMBIGUOUS / baseline NOT_MEASURED.
Nothing pushed, packaged or submitted to fleet intake. The native startup crash
and all arm/GPU/HMD acceptance gates remain open. No further launch was taken.
