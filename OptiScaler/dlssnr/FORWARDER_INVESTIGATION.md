# Removing the forwarder — evidence log

The forwarder (`nvngx.dll_dlssnr.dll`) exists only to satisfy the snippet's caller check: the model
resolves its caller's module via `RtlPcToFileHeader` and rejects anything whose path does not contain
`nvngx.dll`, with `FAIL_PlatformError`, before looking at a single argument. Naming the shim
`nvngx.dll_dlssnr.dll` gets past that.

The way to remove it is the **proxy path**: don't call the snippet directly, call the driver core's
`NVSDK_NGX_D3D12_CreateFeature(18)` and let the core call the snippet — the snippet then sees the core
(`_nvngx.dll`) as its caller and the check passes for free. This is how RenoDX avoids a forwarder: it
detours the core's Create/Evaluate rather than calling the snippet itself.

The original proxy experiment failed at feature creation with
`0xBAD0000B FAIL_UnableToInitializeFeature`. This error alone does not establish that the core
loaded the NR snippet or explain why creation failed. The historical observations below have not
been reproduced after the ABI corrections described next.

## Maintainer-feedback implementation (2026-09-11)

The experimental backend uses OptiScaler's existing `NVNGXProxy` initialization,
capability-parameter, create, evaluate, release and destroy entry points. The direct
forwarder remains available because successful driver-dispatched NR rendering has not
been verified in a game.

Code inspection and a local MSVC 14.44 x64 assembly probe against the repository's actual
`nvsdk_ngx_params.h` found that the old proxy was guessing virtual-table slots from
source declaration order. The compiler emits these calls for the SDK's typed setters:

| Parameter type | Emitted dispatch | Vtable slot |
| --- | --- | --- |
| `float` | `[rax+48]` | 6 |
| `unsigned int` | `[rax+32]` | 4 |
| `ID3D12Resource*` | `[rax+8]` | 1 |

Thus slot 6 for floats does not demonstrate a private or incompatible driver ABI. The
old unsigned setter called slot 3 (the signed integer overload), and the resource
setter called slot 0 (the `void*` overload). The backend now uses typed `Set` calls,
removing the float-slot scan and its calls through incompatible function signatures.
Whether correcting these types resolves NR creation still requires a driver/game test.

The SDK documentation for `NVSDK_NGX_GetCapabilityParameters` says it creates a new
map that the caller must destroy with `NVSDK_NGX_DestroyParameters`. The old description
of this map as borrowed and shared with the game's DLSS confused it with deprecated
`GetParameters`. The proxy now owns and releases its map after releasing its feature.
It also no longer repeats the already-proven ineffective `Init_Ext` call.

The proxy now preserves creation errors, latches failures until explicit retry, and
recreates for creation-time settings, resolution or device changes. Like the existing
direct backend, it waits until the next call to evaluate a newly created feature and
retires replaced feature/map pairs for 32 calls. This matches the current effect's
resource-retirement policy; it is not a substitute for a GPU completion fence.

A focused regression harness compiles the production proxy implementation with mock NGX
entry points and the real SDK parameter interface. From a Visual Studio developer
PowerShell, run `./tests/dlssnr_proxy/run.ps1`. It covers typed parameter values,
creation/evaluation separation, tuning changes, deferred destruction, failure propagation,
explicit retry, and final ownership cleanup. It does not exercise NVIDIA driver behavior.

Each historical entry: what was tried, what the log said, what it rules out.

---

## Historical observations

- `CreateFeature(18)` returned `0xBAD0000B`, while an unknown feature returned a different
  result. This suggests that the dispatcher recognizes feature 18; it does not by itself
  prove successful snippet discovery or that the snippet was called.
- Re-initializing the core with `Init_Ext` returned success but logs retained the original
  application ID and SDK version. The experiment did not change the existing NGX session.
- Float values round-tripped through slot 6. This agrees with the SDK interface as compiled
  by MSVC; it does not establish that the remaining manually selected slots were correct.

## Theories tried and disproven

### Warm-up retry — DISPROVEN (2026-09-01)
Feeder projects note the feature "re-creates a few seconds in, which normally clears" a failed state.
Tried: retry `CreateFeature(18)` up to 20 times, ~1 attempt / 20 frames, ~1.3s total, in Cyberpunk
with the game's own DLSS running (core fully warm).
Result: all 20 attempts returned `0xBAD0000B`, none succeeded.
Rules out: a transient warm-up window as the cause. The failure is stable, not timing.

## Theories not yet tried

- **Snippet discovery path.** The core loads snippets from the path list it was given at `Init` (the
  game's DLSS directory) plus the app directory. If `nvngx_dlssnr.dll` is not on a path the core
  searches, it finds feature 18 and has nothing to build it from -> UnableToInitialize. Since Init is
  idempotent we cannot add a path after the game's own Init; the snippet would have to sit where the
  core already looks. Test: place `nvngx_dlssnr.dll` beside the exe / in the game's DLSS plugin dir
  and check whether the core loads it (Streamline/NGX log should show the load).
- **Feature registration / discovery step.** The game only ever registers the SR/RR/FG snippets with
  the core, never NR. Feature 18 may need a discovery call (`GetFeatureRequirements` /
  `UpdateFeature`) before `CreateFeature` will build it. Test: call the D3D12 requirements query for
  feature 18 through the core before creating, and see whether that changes the create result.
- **Scratch buffer.** `CreateFeature` may need `GetScratchBufferSize(18)` satisfied first. Untested.

## How to reproduce
Set `[DlssNr] UseProxy=true`. The path is off by default and does not fall back automatically, so a
failure is visible rather than masked by the forwarder quietly doing the work.
