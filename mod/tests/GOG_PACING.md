# GOG frame timer address coverage

On GOG 1.03, `FramePacing=1` could log `frame pacing hook (game timer update):
FAILED`: the existing timer hook passed Steam addresses to `gameaddr::Xlate`,
but the GOG table did not contain the update function or timer singleton.

This patch adds those two mappings in the table's required ascending order:

| Symbol | Steam VA | GOG VA |
| --- | --- | --- |
| Timer update | `0x1029B290` | `0x10299360` |
| Timer singleton | `0x11606360` | `0x11555A60` |

The timer hook, expected prologue, pacing algorithm, configuration and Steam
path are unchanged. The existing patch guard still checks the update entry bytes.
This makes the existing pacing implementation reachable on the supported GOG
build; it does not address every cause of hand, body or vehicle jitter.

## Validation

The `gog_pacing_map` CTest checks that every native address used by the existing
frame-pacing adapter has a nonzero GOG mapping, and that the complete map remains
strictly ordered for the production binary search. Removing either new entry or
reordering the table must fail. This is source coverage, not a native ABI test.

```powershell
cmake -S mod/tests -B build/gog-pacing-tests -A Win32
cmake --build build/gog-pacing-tests --config Release --parallel 4
ctest --test-dir build/gog-pacing-tests -C Release --output-on-failure
```

The two mappings were exercised in a local GOG/Quest 3/VirtualDesktopXR build
whose owner accepted improved vehicle pacing. That build also contained other
changes, so it is not a controlled comparison of this isolated patch. The
upstream-based PR build is compiled and tested offline; it has not had a separate
headset run. Steam and other editions/hardware need maintainer testing.

For a GOG runtime check, use the normal launch path with `FramePacing=1` and
confirm the startup log reports the frame-pacing hook installed. Test normal
walking and driving, pauses/loading and recovery from a slow frame. Log success
confirms hook installation, not visual quality or complete gameplay coverage.
