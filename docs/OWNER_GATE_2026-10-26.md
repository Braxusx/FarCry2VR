# Exact-image and pose/weapon ownership — offline checkpoint

Branch: `codex/tracking-arms-physical-steering`. Starting parent: `8f0d63b`.
Work remains local; no game, injection, deployment, packaging, push or PR.

## R9 image gate

The former detector accepted PE timestamp or image size and returned unchanged
Steam addresses on unknown images. It now measures the complete on-disk SHA-256
and byte size, parses bounded x86 PE headers and compares mapped/file metadata.
Only the exact Steam/Definitive image currently has an enabled profile:
20,183,176 bytes; SHA-256
`6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765`.
The production `InspectFile` function independently measured this local installed
file, agreeing with PowerShell Get-FileHash. Reading it did not execute game code.

Unknown, malformed, missing, changing or unmeasured identities get no native
addresses. Translation is also bounded to the supported module image range.
Initialization refuses before engine hooks, XInput patching, affinity/DPI changes,
exception/OS hooks and watchdog startup; the system dinput8 proxy still forwards.
An unapproved startup cannot later install native hooks via the proxy entrypoint.
Independent controls/arms installation guards provide another boundary. Existing
hook-byte checks remain; full file identity is not a claim of in-memory tamper
resistance or a proof of every native call's ABI/signature uniqueness.

SHA-256 is a portable streaming implementation, avoiding crypto-provider loading
under the loader lock. Initialization does not wait on another measuring thread:
a concurrent/incomplete observer receives no native authority. Startup timing and
loader behavior have not been accepted in-game. No hook scheduling was moved to a
new worker thread.

**Compatibility limitation:** the GOG translation table is retained, but GOG is
not enabled by this checkpoint. The available corpus does not establish the full
hash, file size and provenance of the specific image used to generate this table.
A timestamp/size or a sibling project's partial digest does not establish that
binding. An exact GOG image plus table checks is required to enable it; do not
label all GOG releases unsupported by the upstream mod. Modernized and other
patched/unknown images are refused as well. There is no unsafe bypass or guessed
offset override in this donor integration.

Tests exercise SHA vectors/chunk/padding boundaries, all digest bytes, wrong size,
same-timestamp variants, malformed/truncated PE headers and unknown translation.
Detailed final counts/build identity follow the ownership checkpoint below.

## Ownership work

The camera callback now observes the current weapon **before** querying that
weapon's graphics/model and before hand interactions. Observation is independent
of `HandGrab`. Previously it queried the previous weapon's model first, published
that with the next weapon, and skipped observation entirely with grabbing disabled.
This ordering is visible in parent `8f0d63b501c3320098159273572f3ca82f90d42d`;
no target execution was required to establish the source defect.

An observed cohort includes pawn/data/graphics, player rig/palette/count/helper
slots and weapon/handle/entity/graphics/model/palette/count/attachment. A change,
missing tuple or producer gap starts a new epoch; A→B→A does not revive A's lease.
The owner heartbeat expires after 250 ms. Continuous observations refresh that
heartbeat, **not** the original XR source receipt (100 ms for physical poses).
Consumers also reread the existing pawn, graphics, palette, count, handle/entity
and attachment fields before using a lease. A readable pointer is not a lifetime.

Aim/scope and monocular rays, final-palm reads, arm/reload/roll/free-hand caches,
support bindings, pending throws and cached NPC drive now reject an observed owner
change. Final palms publish only after palette writeback and keep their source
stamp. The old pointer-keyed hold and frozen-weapon palettes were removed; offsets
and bones recapture per cohort. Loaded offsets/holds reject nonfinite/nonrigid
matrices. Frozen palettes publish bounded finite copies under the mod lock and
replace only an update map covering every saved bone, not a partial palette job.
Monocular render-history reuse is also fenced by observed cohort and model pointer.

HUD clip maxima and reload hints are cohort-local, rather than retained by reticle
ID. The reload hook validates the observed pawn/data pair but no longer retains
and dereferences the native reload-state pointer after its callback. HUD timing,
learned animation IDs and the seven-second timeout remain cosmetic hints, **not**
request/weapon acknowledgement. Pending throw metadata and owner publish together.
An owner change cancels queued gestures. Pose-guided trigger output requires a
current observation and neutral-trigger rearm; a cached XInput sample releases
the trigger if observation expires between XR samples. Existing native vehicle,
mounted and menu-map paths retain their previous behavior.

## Evidence, build and reproduction

- MSVC x86 Release: all 48 CTest groups pass, 83,935 numeric assertions plus five
  source-wiring guards. New owner groups: 473 checks. Image groups: 1,485 checks.
- GCC 16.2.0 x86 (MSYS2 Rev3): the four owner groups and four image groups pass with
  `-std=c++17 -O2 -Wall -Wextra -Werror`.
- Five separately rebuilt mutants fail as expected: omit epoch matching, omit
  heartbeat expiry, allow incomplete palette coverage, ignore the image digest,
  or let unknown images use Steam addresses. Production files were not mutated.
- Full development DLL builds with the installed x86 GCC toolchain: PE32/i386,
  `DirectInput8Create` exported, 31 warnings (same warning messages as arm-rig-1).
  No new crypto/runtime DLL dependency was introduced. The documented llvm-mingw
  toolchain remains unverified; launcher/DLSS helper were not rebuilt.
- `build/owner-gate-1/dinput8.dll`: 1,310,447 bytes; SHA-256
  `8c0db2267313d2ee482f7b4c185c3435a391197176b46eba484ce11b73df011c`.
  This is an undeployed development artifact, not a release package.

Reproduce from this checkout:

```powershell
cmake -S mod/tests -B build/tracking-tests-msvc -A Win32
cmake --build build/tracking-tests-msvc --config Release
ctest --test-dir build/tracking-tests-msvc -C Release --output-on-failure
```

Compiler/build output and isolated mutant fixtures remain in ignored
`build/owner-gate-1/`. Durable CTest and file-measurement output is in
`docs/evidence/owner-gate-1/`. Source guards check adapter topology only, not
native execution. A standalone research receipt uses the fleet schema and is
validated locally; this donor checkout is not registered in `sources.yml`.
No fleet configuration/intake, graph, remote or release was changed.

## Still open / next acceptance

This is a bounded R4/R9/R10 safety slice, not completion of native ownership.
The mod lock serializes its published records/direct copy section, not the engine's
allocator or jobs. Unobserved identical-address reuse (ABA), native object lifetime,
worker/write ordering, and whether palette hooks consume the intended frame need
separate proof. No engine reference/lease was invented. Existing mutable globals
and game-input callback threading have not been converted to a new scheduler.

Legacy saved-hold keys still use animated topology/lengths and are not definitive
asset identities. Matrix validation and cohort scoping do not establish saved-hold
provenance. Monocular attachment/visibility lifetimes and NPC target/driver leases
remain separate native concerns. There is no native reload ticket/weapon identity
acknowledgement and no usable manual-reload feature added here.

Strict cohort checks can discard a custom throw override when native grenade
equip/reload changes the weapon/model; native launch is the fallback. First runtime
acceptance should cover reload/draw/throw, phone/map/monocular, holster/weapon swap
while holding grip/trigger, loading/recenter/focus loss and long producer pauses,
including `HandGrab=0`. Confirm epoch logs, neutral rearm, no stale pin/aim and
normal native vehicle input. Startup acceptance must check the exact supported
image and an explicitly isolated unknown-image proxy-only control.
`ArmRigFix=0` remains default; wrist/elbow native/mesh/headset acceptance is still open.
No game was launched in this batch. Revert the owner-check commit independently
if native acceptance fails; revert `1a5bc7a` only with explicit awareness that doing
so restores unsafe unknown-image inference. No live installation has been changed.
