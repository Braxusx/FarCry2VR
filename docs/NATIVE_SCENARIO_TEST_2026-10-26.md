# One native headless launch — 2026-10-26

**Result: FAILED startup; INVALID arm-sweep experiment.** Exactly one game
process was launched. No retries. No scenario trace, palette captures, eye BMPs
or success review bundle were produced. The game is stopped; original files and
all five existing saves were independently hash-verified after restoration.

Source: `5d6e4c15507352f63dbb5b2edb2631b588031297`; branch
`codex/tracking-arms-physical-steering`. Raw native evidence is local/ignored at
`out/native-scenario-20261026-01/`; temporary configs, backup binaries and saves
remain under ignored `build/native-scenario-20261026-01/`. Do not add them to Git.

## Intended discriminator

Question: can the v2 runner collect all 22 independently correlated stereo arm
samples, then pass its file/provenance checker and generate a review bundle?
The existing CPU controls are the positive/negative instrumentation controls.
This was one opt-in named-rig native attempt, **not an A/B against vanilla or
upstream**. Startup had to reach an observed on-foot owner and an acknowledged
virtual-XR fixture. Failure before those prerequisites leaves the arm question
unanswered, not refuted.

The exact debug DLL was temporarily substituted for the installed proxy. The
existing INI/Holds were preserved in backups; the test INI requested ArmRigFix=1,
ArmIK=1, 90-degree wrist limit, DLSS/AFW/scope off, no overscan, fixed height
reference, and unrelated grab/contact/melee actions off. Game profile: windowed
1280x720, no MSAA, autosave off. A private x86 xr-sim directory supplied an 800x800
per-eye boot profile with explicit asymmetric signed FOVs. Declared save was a
copy of `5396398652962.sav`; **it was never observed loaded**.

## What actually ran

| Observation | Evidence boundary |
| --- | --- |
| PID 118964, native FarCry2.exe path, created 2026-10-26 04:01:45.7695749 UTC | Observed independently while alive; the shell supervisor failed its own PID-binding step |
| Expected `scenario-runner-2`, `scenarioTesting=1` startup marker and loaded bin proxy path | Actual game log, not the DLL we merely intended to load |
| Steam exact-image gate accepted, including mapped-header agreement | Engine SHA-256 `6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765`, 20,183,176 bytes |
| Host EXE is x86 and LARGEADDRESSAWARE | Read-only PE inspection; host SHA-256 `b5e081fe22c09618eeee68e0e470ce29c7e539100faf849c926b77a4e2fb4db8` |
| Actual runtime `xr-sim 1.0.0`, D3D11 binding, requested system/profile, 800x800 eye targets, controller action setup | Native initialization evidence; not accepted controller gameplay |
| Runtime snapshot: three waits, two begins, two ends, two layered frames, two projection views, zero reported XR errors | Partial submission telemetry only; no saved pixels and no verified game-world stereo |
| Native exception at 04:03:19.373 UTC, followed by self-termination with code 0 | A clean exit code alone would misclassify this failure |

The runtime log queued FOCUSED after VISIBLE, but the last serialized snapshot
still says VISIBLE. Do not turn this older snapshot into a claim that focus never
advanced, or the later runtime event into proof that the game accepted inputs.

## Native failure: bounded static follow-up

The mod recorded read access violation `c0000005` at **preferred VA
0x10189c0c / RVA 0x00189c0c**, access address `0x0000012c`. The logger deliberately
normalizes Dunia addresses to its preferred 0x10000000 base; it is not reporting
an unqualified live RVA. First raw-stack candidate: preferred VA 0x108c9086.

Read-only pefile 2024.8.26 / Capstone 5.0.7 inspection of the same SHA-256 image
confirms the faulting instruction dereferences EAX, and that a direct call ending
at 0x108c9086 targets the containing function at 0x10189c00. This is a useful
callback/reference-replacement investigation lead, **not a proven field/type,
native ownership cause or patch location**. No full register dump or reliable
unwind was retained; the mod's stack output is a filtered word scan. No arbitrary
null guard, guessed ABI fix or game-code patch was introduced.

Ghidra's available instance belonged to SS2, not this image. It was left alone;
the static check used the exact local PE instead. No second game was launched.

## Test-driver defects exposed

1. The synchronous `cmd /c start` invocation in the supervised output pipeline did
   not return until the game exited. The likely mechanism is inherited redirected
   output handles. As a result the supervisor never reached PID binding, its
   watchdog loop or post-start commands while the game was alive. Its final
   `OwnedPid=0` is a **driver failure**, not evidence that no game launched.
2. xr-sim explicitly logged that it ignored `command.txt` written before startup.
   `cmdSeq=0`, no acknowledged command, default head height 1.6 m and default hand
   following confirm that the requested 1.7 m/fixed-hand/permissive-focus bootstrap
   did not apply. This is deliberate runtime stale-command protection, not a bug
   to bypass.
3. The temporary supervisor caught failure but returned shell exit 0. The structured
   failure and native exception were inspected; exit 0 was not treated as PASS.

An **offline-only next template** is retained beside the failed run, with an
explicit used-directory refusal, nonblocking hidden launch helper with file output,
fresh-PID simulator bootstrap/acknowledgement and failure exit codes. It has not
been game-tested and must not be mistaken for a second successful run. A benign
Python child verified that the nonblocking helper returned in 18 ms while that
child was alive; parser and used-directory refusal checks passed without any game
launch. The exact used script is preserved separately, unchanged.

## Checker and restoration controls

On the actual empty native scenario directory:

- verifier returned **2 / NO_DATA**;
- review builder returned **1 / INVALID: NO_DATA** and created no output directory;
- no mechanical, visual, native-palette or HMD acceptance was assigned.

The game terminated itself; it was not force-killed. No FarCry2 process remained.
Independent checks matched original hashes for the proxy, INI, GamerProfile,
current/previous installed logs and all five save files. Process-scoped environment
selection was restored; no runtime registration or global simulator state changed.
The game's raw crash logs were copied before the installed logs were restored.

## Next step — not performed here

Do not repeat the blocking launch/preseed workflow. First use the corrected
identity-first supervisor and require the **fresh** bootstrap acknowledgement.
Investigate the native front-end callback crash with bounded register/caller
observations and a comparable bootstrap control before retrying the arm sweep.
This run cannot distinguish a contribution regression, upstream bug, virtual-XR
interaction or another initialization issue. Another game launch needs separate
authorization. All v2 arm/temporal/GPU-consumer/HMD acceptance gates remain open.
