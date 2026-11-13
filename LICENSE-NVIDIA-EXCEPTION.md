# Additional permission for NVIDIA DLSS (GNU GPL version 3, section 7)

Far Cry 2 VR is licensed under the GNU General Public License version 3 (see `LICENSE`).

NVIDIA DLSS runs in a separate 64-bit helper program, `fc2vr_dlss_host64.exe`, built from `mod/src/dlss_host/`. The mod itself
(the 32-bit `dinput8.dll`) contains no NVIDIA code; it talks to the helper as a separate process.

As an additional permission under section 7 of the GNU General Public License version 3, the copyright holder of
Far Cry 2 VR (Braxus) gives you permission to link or combine that helper program (`mod/src/dlss_host/` and the files it
shares with the mod, `mod/src/dlss_ipc.h`) with NVIDIA's DLSS (NGX) SDK libraries, including `nvsdk_ngx` and
`nvngx_dlss.dll`, and to convey the resulting work. The NVIDIA libraries are governed by the NVIDIA RTX SDKs License
Agreement and not by the GNU General Public License; the Corresponding Source of the combined work does not need to
include their source code.

The helper is based on the helper of the Dishonored VR mod by Pizza Parker, used with the author's permission (its own
credit notes are kept in the source). This permission is given by Braxus for his own contributions.

This permission does not extend to third-party components, which keep their own licences (see
`THIRD_PARTY_NOTICES.md`). If you modify this program you may extend this permission to your version, but you are not
obliged to. If you do not wish to, delete this file and the reference to it from your version.

NVIDIA's own files are not part of this repository. `nvngx_dlss.dll` is proprietary software of NVIDIA Corporation and
is NOT covered by the GNU General Public License; it is redistributed only inside the mod's releases, under the NVIDIA
RTX SDKs License Agreement (copy: `docs/NVIDIA-RTX-SDKs-License.txt`).
