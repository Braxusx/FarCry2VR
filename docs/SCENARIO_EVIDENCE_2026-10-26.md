# Scenario evidence v2 — local/offline checkpoint

Branch: `codex/tracking-arms-physical-steering`. Parent: `a51b860`.
No game, XR session, graphics fixture or headset was launched. Nothing deployed,
packaged, pushed or submitted as a PR. `ArmRigFix=0` remains default.

## Question and decision rule

Can the existing raw-input runner collect bounded temporal evidence and reject
misleading comparisons without bypassing production authority? The controls are
explicitly synthetic files and the existing portable runner/encoder. Variables
are incompatible provenance, mixed applied poses, missing/repeated samples,
stale clocks, changed artifacts and removed observation gates. Reject these;
keep valid observations separate from visual, native-consumer and HMD acceptance.

This refines [v1](SCENARIO_RUNNER_2026-10-26.md), using the useful evidence patterns
reviewed in SWAT4VR, Sims4VR, PreyVR and SSR-VR. It does not transplant their engine
backends. SSR's executed mock flows are not retail-FC2 evidence. No new native
address, field, call or injection seam was introduced.

## Provenance and comparison

`scenario-runner-2` headers identify the host executable, Dunia image and loaded
proxy module **files** by full SHA-256 and size; the existing exact-image startup
gate remains in force. Reported XR runtime name/version, effective ArmIK,
ArmRigFix and wrist limit are recorded. The startup INI is snapshotted into
`config.ini`, hashed and checked again before starting and at termination.
INI/save/profile files changing during a run prevent valid completion. Hashing
is bounded and rejects files changing while read. No settings are written.

Three additional process variables are required for future activation, alongside
the existing case, explicit arming token and existing absolute output directory:

| Variable | Meaning |
| --- | --- |
| `FC2VR_SCENARIO_FIXTURE` | 1–80-character scene/save label: letters, digits, `_`, `-` |
| `FC2VR_SCENARIO_SAVE` | Absolute path to the disposable save file declared for this run |
| `FC2VR_SCENARIO_PROFILE` | Absolute path to the graphics-profile file declared for this run |

These hashes are **declarations, not proof that the game loaded those files**.
Do not invent a native save/scene acknowledgement. A future operator must verify
the scene, avatar and weapon setup. Matching skeleton name/parent fingerprints
checks topology, not mesh/skin-weight identity or native lifetime.

Configured testing stays passive at loading screens/menus until a tracked,
observed on-foot owner exists. It does not clear real menu input or start the
ten-second observation watchdog before that prerequisite. It neither loads a
save nor navigates menus. Once started, all previous context/neutral-drain guards
remain. An arm sweep additionally requires ArmIK=1. A missing prerequisite is
not a successful test or proof of a regression.

The checker requires v2 metadata; old receipts are historical, not silently
upgraded. Comparison refuses different environments, target/runtime, declared
save/profile/scene, topology, render policy, FOV, IPD or mismatched sample coverage.
It checks anchors/recenter and actual sampled eye poses, plus the baseline native
model-to-world frame. Pointer addresses and process-local epochs are not compared
across independent runs as if they were persistent identities.

Default comparison tolerances are reported in JSON: XR position/recenter
components 1 cm, quaternion orientation 0.5 degrees (q and -q equivalent), IPD
0.1 mm, FOV 1e-5 radians, motion progress 0.025, native baseline translation 2 cm
scaled to game units and native basis components 0.01. These are fixture gates,
not perceptual-quality thresholds. Rejected comparisons need a better-matched
fixture; pixel differences alone never provide a correctness verdict.

Only explicit, allowlisted experiment changes can differ: `mod`, `ArmRigFix`,
`ArmWristTwistLimit`. INI snapshots are parsed case-insensitively without
interpolation; duplicate/ambiguous entries are rejected. Allowing a changed DLL
does not allow unrelated INI changes. Named setting allowances cover both their
effective value and corresponding `[VR]` entry. Changed hashes remain visible.

## Timestamped sampled capture

Each moving wrist/reach phase now requires five independent stereo observations,
approximately at progress 0, .25, .5, .75, 1. Arm sweep has **22 pairs** including
baseline/final neutral. Tracking loss, held recovery and neutral rearm each have
three samples: **10 pairs** with baseline. Weapon swap retains three state pairs.

Each sample has its own capture ID. A prior sample acknowledgement cannot complete
the next. Missing the target observation window by over 200 ms fails instead of
batching endpoint images under intermediate labels. This does **not** relax the
normal 100 ms input/pose freshness rules. The complete plan still has its 60-second
bound and neutral entry/exit drain. Repeated endpoint frames are not motion proof.

Trace records include original receipt milliseconds and XR predicted display
nanoseconds. Both eyes must agree on the application frame, source, generation,
owner, applied-pose source/frame, topology, physical/configured-rig state and XR
context/time, source texture format and dimensions. The portable native pair gate now includes these identities; the
offline checker independently validates them and reports both eye pose-frame gaps
and the applied source's recorded time/progress when available.

The adapter records actual XR eye poses/signed FOV, scale/render policy and
recenter basis from the source-owned render context. Owner/source/applied-palette
observations bracket GPU readback and encoding; a changed context or expired lease
aborts. Four unsuccessful pair-frame attempts remain bounded. A run has a 1 GiB
BMP budget; existing 4096-square/format/finite-value bounds remain. Readback and
file I/O can stall the game; these samples are not performance measurements.

Images remain **game eye targets before DLSS**, not final XR composition. AFW,
DLSS and scope paths are refused for these captures, not silently disabled or
replaced. Captured XR FOV is not proof of the game's final GPU projection.
Palette-writer snapshots still do not identify the GPU skinning consumer or
prove native animation/worker lifetime. Those gates remain open.

## Offline commands (never launch a game)

Use a verified Python 3.10+ interpreter. The following consume existing receipts
and create new local review output only:

```powershell
python mod/tests/verify_scenario_run.py '<run>'
python mod/tests/verify_scenario_run.py '<baseline>' --compare '<candidate>' --allow-change mod --allow-change ArmRigFix
python mod/tests/review_scenario_run.py bundle '<run>' --out '<new-review-directory>'
python mod/tests/review_scenario_run.py compare '<baseline>' '<candidate>' --out '<new-comparison-directory>' --allow-change mod --allow-change ArmRigFix
python mod/tests/review_scenario_run.py check-bundle '<review-directory>'
```

Bundles contain copied, reverified originals, a hash manifest, source report,
stereo PNG previews and `REPORT.html`. Its manual replay uses intervals between
the recorded XR timestamps; a slider exposes every sample. It is sampled replay,
not continuous video or measured playback FPS. Comparisons provide separate
left/right before/after/absolute-difference contact sheets. PNG thumbnails are
review aids; original-resolution BMPs remain the pixel evidence. No browser is
opened, no ffmpeg/dependency download is needed, and existing output is refused.

An explicit image review is separate from the immutable mechanical receipt:

```powershell
python mod/tests/review_scenario_run.py record '<run>' --out '<new-visual-review.json>' --reviewer '<name>' --verdict PASS --notes '<actual sampled-image observations>'
python mod/tests/review_scenario_run.py check-review '<run>' '<visual-review.json>'
```

The exclusive-create record binds reviewer, notes, scope, environment and verdict
to the report and all artifact hashes. Reverification detects changed evidence.
Failed/incomplete mechanical runs cannot acquire a visual PASS. A visual PASS is
only a review of those sampled images; it never changes `headset_accepted:false`.
The tool does not authenticate a person's identity. Failed/partial raw runs are
preserved; this version refuses to certify/bundle them as successful evidence.

## Offline verification and next acceptance

All **58 MSVC Win32 Release CTest groups** pass, including **16 Python receipt
methods and 13 review methods**. Seven portable groups also pass GCC x86 with
warnings as errors. Three independently rebuilt mutants fail through assertions:
mixed applied poses, endpoint-only sampling, and old-sample acknowledgement.
Both DLL flavors build as PE32/i386 with the proxy export and debug-payload
inclusion/exclusion checks. Both retain 31 existing warnings, none added.
Build gate inspection includes ASCII **and UTF-16** environment-name strings.

Reproduction: normal CMake build/CTest from v1; Python tests are registered in
CTest. `mod/tests/check_scenario_mutants.ps1 -Out '<new-absolute-path-inside-checkout>'`
builds only owned CPU tests. `mod/build-local.ps1 [-ScenarioTesting]` builds isolated
`build/scenario-production-2/` or `build/scenario-debug-2/`, never deploys them.
Durable logs and exact DLL identities: `docs/evidence/scenario-runner-2/`.

No new game pixels, runtime input acceptance or wrist/elbow visual quality were
measured. First native acceptance must be separately authorized: verify the
loaded v2 marker/exact image, declared disposable fixture and supported render
profile; validate one case and inspect both eyes/temporal samples. Retain failures
and investigate capture stalls or native-consumer timing rather than loosening
freshness or sampling gates. An agent-owned test must still close its owned game
instance gracefully and confirm exit. No manual reload, steering or DQS was added.
