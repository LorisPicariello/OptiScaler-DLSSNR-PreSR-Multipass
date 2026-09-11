# Neural Rendering upstream review

Baseline: official OptiScaler `d36a078fb79a7a36e5dd356e4e173fdd9c7e29ec`.
Cleanup branch: `codex/nr-upstream-cleanup`, created from `c543ccb8`.
The original integration branch remains available. This is a source cleanup, not a rebase or release deployment.

## Neural Rendering changes

- Per-upscaler DX12/Vulkan shader and model ownership, shared setup/dispatch pipelines, and DLSS DX12 bridges for DX11/Vulkan.
- Standard NVIDIA NR runtime/forwarder and optional driver proxy; pre/post-SR placement, multipass, guide/subrect handling, scaling and color composition.
- Every-frame deferred residual SR, residual-across-RR, finished-picture processing, ordinary NR frame hold, exposure/calibration, capture and comparison controls.
- NR resource/queue hooks, lifetime/status handling, Vulkan extension negotiation and focused regressions.

Removed: fork-specific Ada/Ampere/Turing MFG unlockers, patched capability ceilings, external-FG ownership mode, residual-FG interpolation, approximate cameras, its missing-motion two-frame fallback, and optional FG runtime downloads/distribution. NVFP4 remains absent. Legacy keys are ignored on load and removed on save; ordinary FG/NR/user keys remain intact.

## Retained compatibility changes

These are separable benefits, retained intentionally rather than presented as NR algorithms:

- DXGI window-sized/composition swapchains and safe composition-window association.
- Vulkan overlay framebuffer destruction and FG/menu interlock.
- Streamline binding against active initialized plugins, guarded capability queries, and runtime-reported frame-count limits.
- KCD2 waitable-swapchain and process-local Streamline OTA compatibility handling (no saved driver profile modification).
- API-specific DLSS/RR initialization, submitted initialization uploads, optional bridge-resource clearing and state/parameter restoration.
- Output scaling sized from actual resources with explicit filters; safe unformatted tooltip rendering.

Official FG backends and hardware capability checks remain. Startup, LibraryLoad and Reflex ownership code matches the baseline again. Official version metadata is restored. Existing official Vulkan-DX12 submission/abort safety and DX11 binding preservation were retained.

## Validation

- Release x64 solution build: passed, 64 warnings and zero errors. Warnings include existing conversion/inheritance, alignment and default-library diagnostics.
- Production proxy/status/pipeline regressions: passed.
- Legacy-settings check using production deletion statements and the repository's SimpleIni: passed; removed keys cleared while normal FG, NR placement/deferred/hold/pass settings and unrelated user settings survived. Source scan confirms no legacy option reads or runtime members remain.
- Guide metadata, seam scheduling, NGX handle/optional-input routing, active-color rectangles, GPU timing and DXGI window/composition smoke tests: passed.
- WARP skin/deferred residual and HDR finished-picture tests, plus production Vulkan shader execution on RTX 5090: passed. Generated header bytes match the compiled shaders; surviving operation IDs remain stable.
- NR package smoke: passed; 39 checksummed files verified, with no NVIDIA model, DLSSG, Streamline or removed unlocker payloads. Packaging uses direct dependency sources and explicit file inclusion.
- Project compile registrations and absence of removed runtime/UI references: passed. Obsolete option names remain only for INI migration and explanatory documentation.

In-game behavior, NVIDIA model output and ordinary FG interoperability were not exercised by this cleanup. Existing fixed-delay NR retirement, finished-picture timeout handling and single-device helper limitations remain documented; this cleanup does not claim to resolve them. No game files were changed.

## Complete baseline difference inventory

The table below accounts for every changed/added path relative to the pinned official baseline. Purely removed fork additions do not appear because they did not exist upstream. Shader byte arrays are generated artifacts of the listed NR shaders, not independent features.

| Path | Purpose or compatibility benefit |
| --- | --- |
| .github/workflows/package_release.yml | NR build/install/package integration and artifact hygiene |
| .gitignore | NR build/install/package integration and artifact hygiene |
| docs/COMPATIBILITY-CHANGES.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/CREDITS.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/DEFERRED-NR-DLSS.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/ISSUE-REVIEW-2026-09-10.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/NR-COMPATIBILITY.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/NR-MOTION-METADATA.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/NR-UPSTREAM-REVIEW.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/PADDED-PRESR.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/PR-2-REVIEW.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/PR-REVIEW-20260909.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/RELEASE-v0.7.0.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/RELEASE-v0.7.1.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/RELEASE-v0.7.6.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/RELEASE-v0.7.7.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/releases/v0.6.1-padded-presr.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/releases/v0.6.2-swapchain-fixes.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/releases/v0.7.3.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/releases/v0.7.4.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/releases/v0.7.5.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/RESIDUAL-ACROSS-RR.md | NR instructions, design/history, compatibility evidence or upstream audit |
| docs/VULKAN-PARITY-REVIEW.md | NR instructions, design/history, compatibility evidence or upstream audit |
| INSTALL-DLSSNR.md | NR instructions, design/history, compatibility evidence or upstream audit |
| Licenses/RenoDX_ATTRIBUTION.txt | NR composition attribution |
| OptiScaler.ini | Baseline defaults plus NR options and DX11 bridge help |
| OptiScaler.sln | NR build/install/package integration and artifact hygiene |
| OptiScaler/Config.cpp | NR settings and targeted obsolete-key migration; baseline FG settings retained |
| OptiScaler/Config.h | NR settings and targeted obsolete-key migration; baseline FG settings retained |
| OptiScaler/dlssnr/design/DEVELOPMENT.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/design/frame-hold.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/design/multi-point-anchoring.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/design/pre-sr-multipass.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Capture.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_ExposureScan.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_ExposureScan.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Menu.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Proxy.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Proxy.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Status.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_Status.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr_VkExtensions.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNr.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNrFeature_Dx12.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNrFeature_Vk.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNrFeature_Vk.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/DlssNrPipeline_Vk.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/FORWARDER_INVESTIGATION.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/forwarder/CMakeLists.txt | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/forwarder/dlssnr_forwarder.vcxproj | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/forwarder/README.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/PassProfiles.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/dlssnr/README.md | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp | Guarded active-plugin binding, real FG limits, Vulkan menu interlock and KCD2 compatibility |
| OptiScaler/hooks/D3D12_Hooks.cpp | NR exposure discovery, root-state tracking and finished-picture submission hooks |
| OptiScaler/hooks/D3D12_Hooks.h | NR exposure discovery, root-state tracking and finished-picture submission hooks |
| OptiScaler/hooks/DxgiFactory_Hooks.cpp | Window/composition swapchain compatibility; NR Present, resize and HDR hooks |
| OptiScaler/hooks/DxgiFactory_Hooks.h | Window/composition swapchain compatibility; NR Present, resize and HDR hooks |
| OptiScaler/hooks/DxgiSwapchainSizing.h | Window/composition swapchain compatibility; NR Present, resize and HDR hooks |
| OptiScaler/hooks/FG_Hooks.cpp | Finished-picture NR integration with existing FG presentation |
| OptiScaler/hooks/Kernel_Hooks.cpp | NR Vulkan extensions/HDR metadata and forwarder filename loading compatibility |
| OptiScaler/hooks/Streamline_Hooks.cpp | Guarded active-plugin binding, real FG limits, Vulkan menu interlock and KCD2 compatibility |
| OptiScaler/hooks/Streamline_Hooks.h | Guarded active-plugin binding, real FG limits, Vulkan menu interlock and KCD2 compatibility |
| OptiScaler/hooks/Vulkan_Hooks.cpp | NR Vulkan extensions/HDR metadata and forwarder filename loading compatibility |
| OptiScaler/inputs/NgxFeatureRegistry.h | NGX feature identity, bridge parameters and owned NR lifecycle |
| OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp | NGX feature identity, bridge parameters and owned NR lifecycle |
| OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp | NGX feature identity, bridge parameters and owned NR lifecycle |
| OptiScaler/inputs/NVNGX_DLSS_Vk.cpp | NGX feature identity, bridge parameters and owned NR lifecycle |
| OptiScaler/menu/menu_common.cpp | NR menu/keybinds/comparison/timing; safe tooltip and Vulkan menu behavior |
| OptiScaler/menu/menu_common.h | NR menu/keybinds/comparison/timing; safe tooltip and Vulkan menu behavior |
| OptiScaler/menu/menu_overlay_vk.cpp | Correct framebuffer cleanup and Vulkan overlay lifetime |
| OptiScaler/nvapi/NvApiHooks.cpp | Guarded active-plugin binding, real FG limits, Vulkan menu interlock and KCD2 compatibility |
| OptiScaler/OptiScaler.vcxproj | NR build/install/package integration and artifact hygiene |
| OptiScaler/OptiScaler.vcxproj.filters | NR build/install/package integration and artifact hygiene |
| OptiScaler/OptiTypes.cpp | Expose DLSS DX12 bridge selection for NR |
| OptiScaler/OptiTypes.h | Expose DLSS DX12 bridge selection for NR |
| OptiScaler/proxies/Streamline_Proxy.h | Guarded active-plugin binding, real FG limits, Vulkan menu interlock and KCD2 compatibility |
| OptiScaler/resource_tracking/ResTrack_dx12.cpp | NR exposure discovery, root-state tracking and finished-picture submission hooks |
| OptiScaler/resource_tracking/ResTrack_dx12.h | NR exposure discovery, root-state tracking and finished-picture submission hooks |
| OptiScaler/shaders/dlssnr/DlssNr_ActiveColor.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Common.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Dx12.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_GpuTime.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Guides.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_ResidualPair.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_SeamClock.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Vk.cpp | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/DlssNr_Vk.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_color_Shader.cso | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_color_Shader.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_color.hlsl | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_residual_Shader_Vk.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_residual_Shader_Vk.spv | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_residual_Shader.cso | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_residual_Shader.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr_residual.hlsl | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader_Vk.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader_Vk.spv | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.cso | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl | NR model, composition, scheduling, controls or generated shader artifact |
| OptiScaler/shaders/output_scaling/OS_Dx12.cpp | Resource-sized dispatch and explicit NR scaling filters |
| OptiScaler/shaders/output_scaling/OS_Dx12.h | Resource-sized dispatch and explicit NR scaling filters |
| OptiScaler/shaders/output_scaling/OS_Vk.cpp | Resource-sized dispatch and explicit NR scaling filters |
| OptiScaler/shaders/output_scaling/OS_Vk.h | Resource-sized dispatch and explicit NR scaling filters |
| OptiScaler/spoofing/Vulkan_Spoofing.cpp | NR extensions on actual devices; avoid DXGI/Vulkan enumeration recursion |
| OptiScaler/State.h | Track Vulkan overlay lifetime for menu/FG compatibility |
| OptiScaler/upscalers/dlss/DLSSFeature_Dx11.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_Dx11On12.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_Dx11On12.h | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_Dx12.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_Vk.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_VkOn12.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature_VkOn12.h | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlss/DLSSFeature.h | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlssd/DLSSDFeature_Dx11.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlssd/DLSSDFeature_Dx12.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlssd/DLSSDFeature_Vk.cpp | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/dlssd/DLSSDFeature.h | NR-capable DLSS bridges and independent per-API SR/RR initialization |
| OptiScaler/upscalers/FeatureProvider_Dx11.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/FeatureProvider_Vk.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Dx11.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Dx11wDx12.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Dx12.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Dx12.h | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Vk.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_Vk.h | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/IFeature_VkwDx12.cpp | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/NgxOptionalDx12Inputs.h | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/ShaderPipeline_Dx12.h | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/upscalers/ShaderPipeline_Vk.h | Owned NR pipeline integration; bridge initialization, input and state preservation |
| OptiScaler/wrapped/wrapped_swapchain.cpp | Window/composition swapchain compatibility; NR Present, resize and HDR hooks |
| OptiScaler/wrapped/wrapped_swapchain.h | Window/composition swapchain compatibility; NR Present, resize and HDR hooks |
| package_release.ps1 | NR build/install/package integration and artifact hygiene |
| README.md | NR instructions, design/history, compatibility evidence or upstream audit |
| setup_windows.bat | NR build/install/package integration and artifact hygiene |
| tests/dlssnr_proxy/MockNgx.h | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/dlssnr_proxy/ProxyTests.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/dlssnr_proxy/run.ps1 | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/dxgi_window_size_smoke.cpp | Retained window-sized/composition swapchain regression |
| tests/nr_active_color_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_finished_color_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_gpu_time_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_guides_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_ngx_routing_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_residual_dlss_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_residual_rr_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_seam_clock_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_skin_shader_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
| tests/nr_vulkan_shader_smoke.cpp | Surviving NR, bridge, proxy, shader, timing and metadata regressions |
