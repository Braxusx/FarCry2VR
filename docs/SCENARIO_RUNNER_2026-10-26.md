# Debug-only scenario runner — offline checkpoint

Branch: `codex/tracking-arms-physical-steering`. Parent:
`c0f3627e6c194667e0eccf508b5abc9d6739ae6f`. Local only: no game launch,
deployment, release packaging, remote operation, runtime switch or headset test.
Implementation: `f277c18c72c723baf4246a3191bcea520201a92c`.

## Purpose and evidence boundary

The question is whether a small source-owned driver can exercise the existing
input/pose pipeline and refuse completion without downstream observations.
The control is a deterministic, explicitly labelled harness model; the variables
are missing/stale observations, failed safety gates and malformed captures.
The decision rule is to require the requested native change, a current applied
palette and matching stereo evidence where applicable; otherwise time out/abort.
Harness success is not a claim that a native call, renderer or headset accepted it.

This implements the useful part of the
[agent-playtesting workflow](https://strangenewworld.net/have-your-ai-agent-playtest-your-game/):
script input, wait on state evidence, capture before/after and review the result.
It does not import the article's Quest/Docker tools, make native console calls,
load saves, automate the game menu or provide emulator-style frame stepping.
Those need their own proved integration. No existing in-house harness was copied
wholesale or assumed to work in this fork.

## Separation of responsibilities

- `mod/src/common/scenario.h`: portable bounded plans, input fixtures, observation
  predicates, deadlines and neutral drain. No Windows/OpenXR, native pointers or IO.
- `mod/src/common/scenario_capture.h`: bounded bitmap encoding and exact
  ticket/frame/source/owner eye-pair aggregation, also desk-tested.
- `mod/src/scenario_runner.cpp`: compile-gated Windows/XR adapter, output receipts,
  read-only palette/pad observations and D3D10 texture readback.
- `mod/tests/verify_scenario_run.py`: read-only evidence/hash/correlation checks
  and an optional before/after pixel comparison. It never launches the game.

Injection is **before** the production pose cache and button/pose rearm gates.
It replaces raw hands/buttons for an explicitly armed test, not published source
stamps, owner identities, `poseAuthority` or native palettes. Normal focus, sync,
tracked evidence, 100 ms input expiry, 250 ms observed-owner expiry and neutral
rearm stay in force. No native address/signature or game ABI was added.
The production XR adapter's raw acquisition and cache/gate evaluation were split
into two loops to expose that seam; its non-test policy remains unchanged.

Real head/eye poses are never overridden. The canonical `head-relative-v1` fixture
anchors once to the tracked head: grips at (+/-0.20, -0.25, -0.45) metres with a
rigid 3 cm aim offset. It is a declared synthetic fixture, not a claim about a real
Touch controller's grip/aim calibration. The anchor is recorded in the trace.

## Cases

| Case | Stimulus | Required observation |
| --- | --- | --- |
| `weapon-swap` | Neutral baseline, normal right-B press/release, neutral settle | A different nonzero weapon identity **and** observed cohort; fresh gamepad poll/applied pose, before/after stereo pairs |
| `tracking-loss` | Right tracking unavailable with trigger held, tracked return while still held, neutral release | Hand epoch change and physical authority revoked, trigger output zero while lost/held, then neutral rearm |
| `arm-sweep` | Both wrists 0→+200→-200 degrees, unwind/reach outward and back, neutral | Fresh post-writeback palette and pad observations throughout; matching captures at stage endpoints |

Sweep samples advance only once per source sample. Minimum dwell times define a
stimulus; they are not evidence that a native transition occurred. Every step has
an eight-second observation allowance; readiness is bounded to ten seconds and
the entire run to sixty seconds. Missing callbacks, old samples, absent images or
an unavailable second eye cannot yield `complete-observed`.
Some test inputs could discharge a weapon if the fire gate regresses. A future
native run must use single-player, a safe test location and a disposable save.
Swap requires usable weapons in different slots; a missing/native-refused slot
selection is a timeout, not evidence of a mod regression or accepted swap.

## Guards and cleanup

The feature is absent unless compiled with `FC2VR_SCENARIO_TESTING=1` (default 0).
Even that build stays inactive unless all three per-process variables are valid:

```powershell
$env:FC2VR_SCENARIO = 'arm-sweep' # or weapon-swap / tracking-loss
$env:FC2VR_SCENARIO_ARM = 'ALLOW_SYNTHETIC_INPUT'
$env:FC2VR_SCENARIO_OUT = 'D:\Dev Debug\Other VR Mods\FarCry2VR\build\scenario-runs'
```

This is a **future activation example, not a launch instruction**. The output root
must already exist. The adapter creates a new PID/time subdirectory and uses
`CREATE_NEW` for trace/images. It will not overwrite an earlier run. Unknown game
images still refuse startup before the debug adapter can configure.

It requires an observed player/rig/weapon and normal on-foot controls. Loading,
special controls, vehicles, map/face-monocular, focus/sync/view loss, an unexpected
owner change or head movement beyond 10 cm/10 degrees abort the plan. Place
`abort.txt` in the logged unique output folder to stop on the next input tick.
The native swap steps allow the expected owner transition; sweeps/loss do not.

Entry/exit invalidate the normal input generation and drain neutral samples.
Synthetic and drain frames disable hold learning and `[Holds]` writes, including
the **last** neutral frame. Pose/support/rest/twist histories are cleared at the
boundary, without manufacturing an owner change or erasing native-cohort ammo
observations. Failures preserve a terminal receipt even if XR never resumes.
These controls are source-reviewed, not native thread/lifetime acceptance.

The runner does not exit the game or touch other processes. A future agent-owned
test must still quit its owned FC2 instance gracefully and confirm it stopped.
Do not close a user's pre-existing session. No instance was launched in this batch.

## Captures and reports

`trace.jsonl` records input sequence/generation, raw synthetic tracking versus
accepted authority, input callback output, observed weapon/cohort, applied-pose
sequence and native/render application-frame stamps. At capture points it stores
both game-eye BMPs and hashes, plus six actual post-writeback native helper
matrices (left upper/fore/palm, right upper/fore/palm) and their model-to-world
frame. The writer copy is **not** proof of GPU bone-cache ownership or skinning.

Only eyes with one ticket, application frame, input sequence and owner form a
pair. Partial attempts remain evidence; four unsuccessful frame attempts abort.
Textures must be single-sample RGBA/BGRA8 or RGBA16/32 float, at most 4096 square.
Unsupported layouts, allocation/map/encoding/write errors or nonfinite pixels
abort rather than produce a blank success image. HDR uses explicit tone mapping.
Readback is synchronous and may itself expose a publication/owner timeout; no
freshness threshold is relaxed to hide that observer effect.

Captures are of the game's eye render target **before** optional DLSS filtering,
not the compositor's final image. Start native acceptance with AFW/DLSS/scope
disabled: reconstructed/skipped eyes are not silently substituted for native
render evidence. These are state-point captures, not video or a complete visual
sweep review. Intermediate input samples are logged, but must not be called
intermediate GPU/mesh observations.

```powershell
# Read-only, after a separately authorized future run:
python mod/tests/verify_scenario_run.py '<unique-run-folder>'
python mod/tests/verify_scenario_run.py '<baseline-folder>' --compare '<changed-folder>'
```

Exit 0 means correlated observations/files, with `visual_review: required` and
`headset_accepted: false`. Exit 1 is invalid/failed evidence; exit 2 is no trace.
The checker verifies real files, dimensions, hashes, six bounded palette records,
input/poll/owner correlation, observed swap and lost/held/neutral authority states.
The comparison reports changed-channel fractions; it does not judge correctness.
Use the same save, view/anchor, settings and render profile for a useful A/B.

## Offline verification / build

All 57 MSVC Win32 Release CTest groups pass: 114,095 C++ assertions, six source
wiring guards and the six-method Python receipt-control suite. The seven new C++
groups also pass GCC 16.2.0 x86 with warnings as errors. Rebuilt mutants that
acknowledge a requested-but-unobserved swap, skip capture acknowledgements or
accept mismatched/one-eye pairs fail their tests. Python controls additionally
reject absent files/end records, wrong hashes/frame/source/owner, stale/future
pose metadata, nonfinite palettes and incorrect held-trigger rearm.

```powershell
cmake -S mod/tests -B build/tracking-tests-msvc -A Win32 -DPython3_EXECUTABLE='<Python 3.10+>'
cmake --build build/tracking-tests-msvc --config Release
ctest --test-dir build/tracking-tests-msvc -C Release --output-on-failure
./mod/build-local.ps1                 # isolated normal development DLL
./mod/build-local.ps1 -ScenarioTesting # isolated debug development DLL
```

The build-only PowerShell helper verifies PE32/i386, `DirectInput8Create`, and
scenario arming strings/debug symbols present only in the debug variant. Neither
build starts/deploys/packages anything. Both build with 31 existing warnings and
no additional warnings. Exact sizes/hashes are in
`docs/evidence/scenario-runner-1/builds.json`. The llvm-mingw build route is updated
but unverified here; its debug output is separate from the normal packaging path.
No launcher or DLSS helper was rebuilt.

The project-owned standalone research receipt is validated locally. The donor
checkout is not a registered fleet root, so intake/configuration/graph changes
are deferred. Earlier receipts remain sealed at their own source checkpoints.

## Still unproved / first native acceptance

No game ran, no native screenshot exists from this batch, and no wrist/elbow
visual fix is accepted by these tests. Native consumer timing, palette workers,
unobserved same-address reuse, saved-hold provenance and reload acknowledgements
remain open. `ArmRigFix=0` remains default. No reload, steering or DQS feature was
added or enabled. Completion is **observed mechanics + files**, not visual quality.

When runtime testing is separately authorized: verify the loaded debug marker and
exact game image, load a stable disposable save, run one case at a time, validate
its receipt, then inspect both-eye captures. For arm A/B, explicitly compare the
legacy/default path with the opt-in named rig using the same fixture/settings.
Preserve failed/partial runs and correlation gaps rather than relabelling them
PASS. Revert this checkpoint independently to remove the runner; it does not
require reverting the exact-image or observed-owner safety fixes.
