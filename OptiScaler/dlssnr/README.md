# DLSS 5 Neural Rendering (`OptiScaler/dlssnr`)

A self-contained module that drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`, NGX
feature 18) over the frames OptiScaler already handles. Nothing in it is officially supported by
NVIDIA; the model ships in driver packages and is not redistributed here.

## Pipeline integration

`IFeature_Dx12` and `IFeature_Vk` each own a Neural Rendering shader alongside RCAS and output scaling.
The shaders implement `Shader_Dx12` / `Shader_Vk` and receive explicit colour, depth, motion, output
and frame metadata. Shared composition constants live in `DlssNr_Common.h`. The ordinary model,
scratch buffers, history, capture and timing are associated with that shader instance. D3D12's
finished-picture and deferred schedules also belong to the instance. The forwarder separately
caches API initialization and function tables at module scope.

Both APIs use the same `SetupShaderPipeline` / `DispatchShaderPipeline` pattern before and after the
upscaler. D3D11-to-D3D12 and Vulkan-to-D3D12 bridges inherit the D3D12 implementation. Native D3D11
Neural Rendering is not implemented; D3D11 games use the existing interop upscaler.

DLSS SR and RR feature creation already routes through these upscaler classes, including native DLSS
backends. The input hooks no longer dispatch NR separately. Their unrelated NGX passthrough features
remain untouched. Releasing an upscaler releases its NR instance; shutdown clears owned contexts
before the NGX core is shut down.

`[DlssNr] RunBeforeSR=false` keeps the post-upscale placement. Setting it to `true` runs NR through
an owned colour buffer before the game's upscaler, preserving the original colour texture and
restoring its NGX parameter. Both SR and RR+SR use this placement control, including native Vulkan;
RR identity is carried separately for history and experimental residual routing. `BeforeUpscale`
is accepted only as a legacy load fallback when `RunBeforeSR` is absent; configuration saves use
`RunBeforeSR`.

Padded colour allocations use the active render rectangle. Depth and motion-vector subrect offsets
are supported, with motion vectors interpreted at render or output resolution according to their
feature flags. Colour/output composition still requires an origin-zero rectangle. Unsupported
pre-SR colour shapes or extents select the ordinary post-upscale fallback where its output is valid.

`Passes` controls the model stack. Pass 1 uses the base preset, style and tuning; optional `Pass2*`
and `Pass3*` settings specialize later passes. `UnlockPasses` enables up to 30 passes, with additional
style/tuning overrides for passes 4–30. Unset controls inherit the base tuning except that later
passes default local tone to zero. `PassProfiles.h` is shared by the D3D12 and Vulkan implementations.

The D3D12 shader's `ProcessSeam` handles the additional schedules. The pre-seam and post-seam receive
the same final-output identity; ordinary NR shader dispatch is suppressed when a specialized
schedule owns the frame. D3D11/Vulkan bridges supply their submitted-frame epoch, while native
D3D12 uses the Present epoch, so a newly created model is not evaluated before its creation work
has been submitted.

- `FinishedPicture` captures at the upscaler seam and runs the configured later processing through
  command-list, submission and swapchain hooks. Those hooks select a registered, owned shader
  instance rather than a separate global NR pipeline.
- `DeferredDLSS` generates NR's signed contribution before SR, upscales that contribution with a
  private DLSS feature, then composes it after the game's upscaler. It takes precedence over ordinary
  placement on supported SR paths. See `docs/DEFERRED-NR-DLSS.md`.
- `ResidualAcrossRR` with `RunBeforeSR` preserves RR's original colour input and carries NR's edit
  across RR+SR using a reprojected residual history. `ResidualFG` adds an experimental interpolation
  path to deferred processing; these modes have their own pairing and history requirements.

Finished-picture/deferred scheduling is a D3D12 facility, including its API bridges. Native Vulkan
does not implement these schedules and does not substitute a different private upscaler.

The menu receives value-only status snapshots and issues retry/capture requests. It does not own or
retain any GPU resources. A failure in one instance does not replace another instance's model state.

`[DlssNr] UseProxy=true` selects the driver's NGX interface on D3D12 paths. This route uses a dedicated
capability parameter map, the SDK's typed setters, and the same encode/model/resolve stages as the
forwarder route. It does not load the forwarder, and a driver error disables the pass without silently
falling back. `UseProxy=false` remains the default because successful model creation and visual output
through the driver still need runtime verification. Multipass configurations use the forwarder backend
with an explicit log message, because the proxy supports one model context and base tuning only.
Native Vulkan continues to use its forwarder.
Both settings are available in the Neural Rendering menu and are saved with the configuration.
See `FORWARDER_INVESTIGATION.md` for the historical results and the corrected parameter handling.

Ordinary NR remains at the upscaler boundary, before later game UI rendering. Finished-picture mode
is a separate, implemented late path; it must not be described as having the same UI isolation.
There is no general-purpose HUD detector guaranteeing that arbitrary late game UI is excluded.

### Validation

From a Visual Studio developer PowerShell at the repository root:

```powershell
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /p:PostBuildEventUseInBuild=false
./tests/dlssnr_proxy/run.ps1
```

The focused tests compile the production proxy code with a mock NGX backend and the real SDK
parameter interface. They cover typed parameters, independent model instances, creation/recreation,
failure/retry, deferred cleanup, menu-status lifetime, pipeline ordering and bridge parameter
restoration. Additional CPU and WARP smoke tests exercise active colour rectangles, guide metadata,
residual composition, skin controls and scheduling helpers; the Vulkan shader has a separate smoke
test. These checks do not validate a real driver's NR output or every game's resource states.

Ordinary D3D12 feature/texture retirement retains the existing 32-evaluate delay; that is not a GPU
fence guarantee. Deferred/late generations have their own completion and retirement rules. Vulkan
waits for the device before replacing model resources. The forwarder's module-level runtime cache
remains a separate lifetime concern; per-feature shader ownership does not establish unrestricted
multi-device support for that service.

The exposure scanner accepts one device until a GPU-safe shutdown and rejects foreign-device
resources. Finished-picture teardown retains main's five-second wait; a timeout is not proof that
its GPU resources are idle. These inherited lifetime limits still require runtime validation.

## For maintainers: how to remove it

There is no compile switch. Neural Rendering is built with the other shaders and controlled by
the runtime `Enabled` setting, which is off by default.

**The procedure, in full:**

1. Delete `OptiScaler/dlssnr/` and `OptiScaler/shaders/dlssnr/`.
2. Drop `dlssnr_forwarder.vcxproj` from the solution.
3. Remove the integration points below and the `[DlssNr]` block in `Config.h` / `Config.cpp`.

| File | Role |
|---|---|
| `upscalers/IFeature_Dx12.h/.cpp` | owned NR shader and before/after stages, including D3D12 bridges |
| `upscalers/IFeature_Vk.h/.cpp` | owned NR shader and native Vulkan before/after stages |
| `wrapped/wrapped_swapchain.cpp`, `hooks/D3D12_Hooks.cpp` | finished-picture presentation and command submission hooks |
| `resource_tracking/ResTrack_dx12.*`, `hooks/Streamline_Hooks.cpp` | exposure/resource observations and frame metadata |
| `menu/menu_common.cpp` | settings panel and cost row |
| `Config.h` / `Config.cpp` | `[DlssNr]` declarations and configuration read/write |

Also remove the module's project entries and forwarder packaging references. Vulkan integration
includes extension negotiation; search for `DlssNr` references before removing the module.

The config block is contiguous and marked `removable as one block` at both ends, so it lifts out
whole rather than needing to be picked apart.

One change outside the module is **a genuine upstream fix, separable on its own and worth taking
regardless of this feature**: `shaders/output_scaling/OS_Dx12.cpp` sized its dispatch from the global
current feature rather than from the resources passed in. Those coincide for the conventional Output
Scaling chain, so the bug stayed invisible until something else called it.

## Files

The pass itself lives under `shaders/dlssnr/`, dispatched like every other shader here. What stays in
`dlssnr/` contains the menu, status, capture, model backends, exposure scan,
forwarder and proxy route.

| File | Role |
|---|---|
| `DlssNr.h`, `DlssNr_Status.h/.cpp` | hook facades, menu controls and value-only status snapshots |
| `DlssNrFeature_Vk.h/.cpp` | per-instance Vulkan model backend owned by `DlssNr_Vk` |
| `DlssNrPipeline_Vk.h` | Vulkan resource/frame adapters and scoped parameter restoration |
| `DlssNr_Menu.cpp` | the settings panel |
| `DlssNr_Capture.h` | matched before/after frame dumps |
| `PassProfiles.h` | shared per-pass model profiles and tuning inheritance |
| `DlssNr_ExposureScan.h/.cpp` | observed exposure candidates and calibration controls |
| `DlssNr_Proxy.h/.cpp` | the experiment in reaching the model through the driver core instead of the forwarder; see `FORWARDER_INVESTIGATION.md` |
| `forwarder/` | the caller-gate shim, built by `dlssnr_forwarder.vcxproj` into the release layout |
| `shaders/dlssnr/DlssNr_Dx12.h/.cpp` | the pass: forwarder loading, feature lifetime, the evaluate path, encode/resolve orchestration, capture |
| `shaders/dlssnr/DlssNr_DeferredSr.inl`, `shaders/dlssnr/DlssNr_Late.inl` | owned deferred and finished-picture schedules |
| `shaders/dlssnr/DlssNr_Vk.h/.cpp` | Vulkan shader, owned intermediate image and model backend |
| `upscalers/ShaderPipeline_Dx12.h`, `upscalers/ShaderPipeline_Vk.h` | shared setup/dispatch contract used for both placement stages |
| `shaders/dlssnr/DlssNr_Common.h` | the constant buffer, shared by the host and the shader |
| `shaders/dlssnr/precompile/dlssnr.hlsl` | **the live shader**: encode (scale and sRGB-encode with a soft knee), area downsample, resolve (RenoDX's two-branch composition, OkLab hue correction, AP1 clamp, the guard) |
| `shaders/dlssnr/precompile/DlssNr_Shader.h` | that shader compiled, as bytes |
| `shaders/dlssnr/precompile/dlssnr_residual.hlsl`, `dlssnr_finished_color.hlsl` | residual and finished-picture composition shaders |

### Editing the shader

`dlssnr.hlsl` is **precompiled**; editing it alone changes nothing. Rebuild the header:

```
cd OptiScaler/shaders/dlssnr/precompile
../../shader_tools/fxc.exe -T cs_5_0 -E CSMain -O3 dlssnr.hlsl -Fo DlssNr_Shader.cso
python ../../shader_tools/create_header.py DlssNr_Shader.cso DlssNr_Shader.h DlssNr_cso
```

**fxc `cs_5_0`, not the dxc in `build_precompiled_shader.bat` next to it.** Only fxc reproduces the
committed header byte for byte; dxc emits DXIL and would silently change what the pass runs on.
Verified by recompiling the unmodified shader both ways and diffing.

## Attribution

The colour composition -- the two-branch luminance ratio, the OkLab hue correction and the blend
between a luminance-only result and the model's own colour -- is **taken from RenoDX's DLSS 5 addon
by clshortfuse** (https://github.com/clshortfuse/renodx). It is their design, reimplemented here with
different names; that does not make it ours. See `Licenses/RenoDX_ATTRIBUTION.txt`, which must carry
their upstream licence text before any build is distributed.

What is not theirs: the OkLab matrices are Bjorn Ottosson's published constants, and the AP1, sRGB
and PQ transforms are standard colour science.

## Why a forwarder DLL exists

The model's snippet resolves the module that owns its caller's return address and refuses any whose
path does not contain `nvngx.dll`. The forwarder (`nvngx.dll_dlssnr.dll`, ~13 KB) exists only to
satisfy that check; every NGX call to the model originates from it. It contains no NVIDIA code, is
part of the solution, and builds with everything else.

## Design notes worth knowing before changing anything

- **Ordinary composition uses a ratio.** The model is shown an encoded proxy; what it returns is
  composed back as a ratio against the original's luminance, scaled by a measured slope, with the
  chroma added. The deferred and RR-residual schedules then derive a signed difference from that
  composed result; their residual transport is a separate stage with its own numerical guards.
- **Create-time parameters.** The model's tuning (preset, style, intensity, local *) is latched at
  feature creation; changes rebuild the feature after a settle. The driver's parameter block is not
  the SDK header's vtable (floats sit at slot 6); the forwarder probes it. Rebuilding every frame
  exhausts the driver's latches and the feature stops responding until the process restarts, which
  is why the rebuild is debounced.
- **Creation work must execute before evaluation.** Ordinary model creation waits for a later
  submission epoch; bridge initialization explicitly submits its creation command list. Deferred
  schedules additionally track their private generations. Preserve these boundaries when changing
  pass count, resolution or queues, and respect the distinct retirement rules described above.
- **Per-instance locking and hook routing.** Shader state is protected per instance. Late hooks
  use the owner registry, whose lock also protects owner selection against destruction; they are
  not restricted to the ordinary upscaler call site.
- **Residual history is intentional.** Ordinary composition and model histories are distinct from
  the RR residual accumulator, frame-hold reuse and deferred residual FG. Cuts, missed seams and
  generation changes must invalidate the relevant residual pair/history.
- **UI handling depends on placement.** Ordinary pre/post-upscale NR runs before later UI. Late
  finished-picture processing uses different resources and timing. Streamline hooks now supply
  metadata/observations, so older notes claiming this module never touches Streamline are obsolete.
  Semantic skin controls and the model's auto mask are not general HUD detection.
