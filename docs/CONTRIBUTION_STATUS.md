# Contributor checkpoint — height/direction separation

Date: 2026-10-23. Branch: `codex/tracking-arms-physical-steering`.
Base: `92a29c5b1e28a652d067262aacc59b421ed044c1` (upstream 1.0.1).

## Implemented

R5: the former `ToRef` helper added `HeightOffset` to eye and culling direction
vectors. At an offset of 0.2, even a level forward vector became `{0,0.2,-1}`:
approximately 11.31 degrees of unintended pitch, with a nonorthogonal up axis.

`common/tracking_frame.h` now owns two explicit operations:

- `Position`: subtract the XR recenter origin, rotate into its yaw-only frame,
  and apply height translation once. World scale is still applied by the caller.
- `Direction`: rotate only; neither origin nor height translation is applied.

All old `ToRef` call sites were classified and migrated. Arms, calibration,
flashlight and monocular direction conversions share the same helper instead of
duplicating its math. Default `HeightOffset=0` and position behavior are preserved.
No INI keys, native addresses, hooks or animation/interaction policy were changed.
The startup log identifies `contribution height-direction-1`, the loaded module
path, build date/time and configured height behavior. This is not a release version.

## Offline verification

Production helper tested directly by `mod/tests/test_tracking_frame.cpp`;
no copied implementation or OpenXR/game dependency in the normal test target.

| Group | Checks | Coverage |
| --- | ---: | --- |
| position | 15 | Origin subtraction, positive/negative height, yaw, world scale |
| direction | 41 | Origin/height independence, axes, known yaw, old-formula negative control |
| rotation | 365 | Full yaw sweep, length, dot products, orthogonal forward/up |
| stereo | 15 | Asymmetric eye directions and IPD preserved |
| affine | 300 | Displacement consistency, zero-offset behavior, zero vectors |

All 736 checks passed under MSVC 19.50.35729 (Win32 Release, warnings as errors)
and MSYS2 MinGW GCC 16.2.0 (i686, `-O2 -Wall -Wextra -Werror`). Tests use explicit
checks that remain active in Release builds. A separate test executable was rebuilt
against a test-only legacy helper (height added to directions, unchanged position
behavior): the direction group failed 10 checks, exit 1 as expected. That mutant
was never compiled into the mod DLL.

Both unchanged baseline and changed full proxy DLL compiled with GCC using the
source list, optimization, libraries and linker flags in `mod/build.sh`, substituting
`i686-w64-mingw32-g++` for its documented clang compiler. Both produced the same
32 existing warnings (matching diagnostic text after removing line numbers), with
no new warnings. `objdump -p` confirmed PE32/i386, undecorated `DirectInput8Create`,
and only Windows system imports; no dynamic GCC runtime dependency.

The installed GCC compiler requires `C:/msys64/mingw32/bin` on the build process's
PATH for its child executables. No persistent environment setting was changed.
Documented llvm-mingw build, launcher/DLSS helper rebuild, runtime and headset
acceptance remain unverified. The GCC DLL is a development artifact, not a release
candidate. Neither game launch nor deployment was performed.

Local outputs are ignored under `build/height-direction-1/` (DLL, GCC test binary,
build log, legacy control source/binary/log), `build/tracking-tests-msvc/` (CTest),
and `build/baseline-gcc-r5/` (unmodified baseline DLL/log).

Verified development DLL SHA-256:
`758408e2f9bc9aa15da8275641233063f1f0aaabbfe2320dcc9c75b08a20c632`.
Rebuilding changes the compile-time banner and therefore this hash.

## Next acceptance and rollback

Before upstream acceptance, build with the documented llvm-mingw route. With an
explicitly authorized HMD run, compare identical saved poses/configurations at
`HeightOffset=0`, `0.2` and `-0.2`: height should move the view without tilting it,
skewing the eyes or changing tracked weapon direction. Exercise recenter, head
pitch/roll, asymmetric eye views, scope, flashlight and monocular. Preserve and
restore the user's settings, verify the module stamp, and close the owned game
instance after testing. Passing offline math does not establish visual acceptance.

The next implementation slice is R1's shared effective camera frame, so steadied
eyes and tracked hands use the same camera basis/origin. R2 wrist/arm twist,
pose authority, support gripping, vehicle steering and optional DQS remain open.
The full PR 0 and PR 1 gates in the implementation plan are not yet complete.

This is one focused fix commit after the plan checkpoint. Roll back that commit
as a whole if needed; no migration, runtime settings or deployed files need undoing.
No GitHub push, PR, packaging or release has been performed.
