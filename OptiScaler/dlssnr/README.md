# Neural Rendering implementation

This module drives NVIDIA's Neural Rendering model (`nvngx_dlssnr.dll`, NGX feature 18) over
frames OptiScaler already handles. The runtime is supplied separately; it is not redistributed.
Model calls use the NGX core installed with the NVIDIA driver. See the
[installation guide](../../INSTALL-DLSSNR.md) and [attribution](../../docs/CREDITS.md).

## Ownership and integration

`IFeature_Dx12` and `IFeature_Vk` own their NR shader/model instances. GPU resources are created
lazily on first use. NR is off by default. Vulkan must have NR enabled before device/swapchain
creation to prepare optional extensions and transfer usage; enabling it later may need a restart.

The NR pipeline adapters receive explicit color, depth, motion, output and frame metadata.
Shared shader pipelines own only generic ordering and dispatch. D3D11-to-D3D12 and
Vulkan-to-D3D12 bridges reuse D3D12 NR; native Vulkan owns a separate implementation.
Shared codec constants live in `DlssNr_Common.h`.

Model contexts, scratch textures, history, captures and timing belong to the shader instance.
Finished-picture and deferred schedules retain that ownership. Global hook entry points select
live owners; they do not create another rendering pipeline. The menu reads value-only snapshots
and issues control requests without retaining GPU resources.

`RunBeforeSR` selects owned color scratch before SR or combined RR+SR; post-SR is the default.
The adapter restores game parameter bindings and incoming resource states. Active color
rectangles must be origin-zero; depth/motion metadata permits independent subrect origins.
Unsupported pre-SR shapes select post-SR fallback where valid. See
[placement and multipass](design/pre-sr-multipass.md).

`FinishedPicture` captures guides at the upscaler seam and applies at presentation. Native
Vulkan supports its own finished-picture route under the constraints in
[NR-FINISHED-BRIDGES.md](../../docs/NR-FINISHED-BRIDGES.md).
`DeferredDLSS` generates a signed edit before SR, processes it through a private DLSS, FSR 2.2,
FidelityFX or XeSS context, and composes it after the game's SR. Its backend contract carries
explicit guide states, dimensions and camera metadata; it does not change the game-facing
backend factory. Deferred private SR and exposure scanning use D3D12, including its bridges.

## Source map

| Implementation | Responsibility |
| --- | --- |
| `DlssNr_Pipeline_Dx12.*`, `DlssNrPipeline_Vk.h` | NR adapters at the upscaler seams |
| `shaders/dlssnr/DlssNr_Dx12.cpp` | shader dispatch and public per-feature entry points |
| `DlssNr_Dx12_State.h`, `DlssNr_Dx12_ModelState.h` | private owner and model resource/history state |
| `DlssNr_Dx12_Models.cpp`, `DlssNr_Dx12_Resources.cpp` | creation, scratch allocation and retirement |
| `DlssNr_Dx12_Run.cpp`, `DlssNr_Dx12_Encode.cpp` | frame orchestration, encoding and resolve constants |
| `DlssNr_Dx12_Evaluate.cpp` | NGX parameter adaptation and across-RR application |
| `DlssNr_Dx12_Exposure.cpp`, `DlssNr_Dx12_Hold.cpp` | exposure/calibration and frozen inputs |
| `DlssNr_Dx12_DeferredSr.cpp`, `DlssNr_Upscaler_Dx12.cpp` | private SR generations and runtime adapters |
| `DlssNr_Dx12_Late.cpp`, `DlssNr_Dx12_FinishedQueue.cpp`, `DlssNr_Dx12_FinishedCompose.cpp` | guide capture, submission tracking and presentation |
| `DlssNrFeature_Vk.cpp`, `_Model.cpp`, `_Resources.cpp` | native Vulkan frames, model lifecycle and image/readback resources |
| `DlssNr_ExposureScan.cpp`, `DlssNr_ExposureReadback.cpp`, `DlssNr_ExposureAnchors.cpp` | candidate discovery, GPU sampling and calibration persistence |

The `DlssNr_Dx12_*` and private-upscaler files are under `shaders/dlssnr`.
These are real C++ implementation units sharing private declarations, not implementation includes.
The frame orchestration, owner declarations and native Vulkan presentation remain cohesive files
slightly above the approximate 500-line target. Generated shader arrays are exempt. The main HLSL
codec remains one cohesive kernel/constant-buffer contract shared by DXIL and SPIR-V; splitting it
solely to reduce line count would add shader-build dependencies without changing ownership.

## GPU lifetime

D3D12 records completion markers for NR-owned GPU work. Texture/model retirement waits for
completion and discarded command recordings are tracked separately. CPU frame counts only
coordinate logical frames. If teardown cannot prove outstanding work complete, it abandons the
still-referenced owner instead of freeing resources the GPU may use. Deferred generations and
finished-picture slots retain their own completion markers/fences.

Vulkan drains the device before replacing model resources or filter pipelines. Creation events
separate NGX uploads from evaluation. Per-feature ownership does not establish unrestricted
multi-device support in the installed NGX core. The exposure scanner accepts one device until
GPU-safe shutdown and rejects foreign-device candidates.

## Validation

From a Visual Studio developer PowerShell at the repository root:

```powershell
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /p:PostBuildEventUseInBuild=false
./tests/dlssnr_proxy/run.ps1
./tests/run_nr_gpu_lifetime.ps1
./tests/run_nr_private_upscaler_smoke.ps1
```

The proxy regressions exercise production adapters using a mock NGX backend and the SDK parameter
interface. The private-upscaler runner documents its runtime-loader substitutions and installed
runtime requirements in [its test notes](../../tests/nr_private_upscaler_smoke.md). Additional
CPU/WARP and Vulkan smoke sources cover guide rectangles, padded color, residual/skin composition,
HDR transfer, pairing and presentation. Synthetic texture tests establish those specific contracts,
not moving-scene quality or full game/driver stability.

Shader edits must regenerate both DXIL and SPIR-V arrays from the same constant-buffer layout.
Keep all `.cpp` files using `pch.h` as their first non-comment include, as required by
[CONTRIBUTING.md](../../CONTRIBUTING.md).
