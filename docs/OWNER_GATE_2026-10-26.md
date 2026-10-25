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

In progress in this local batch. Remaining baseline risks include late weapon
observation (the old model is used before querying the new weapon), pointer-only
hold/rest caches, indefinite last-palm use and unscoped reload/attachment records.
No ownership completion is claimed by the image-gate commit alone.
