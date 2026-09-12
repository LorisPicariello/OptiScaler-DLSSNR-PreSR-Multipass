# Native Vulkan NR integration

The Vulkan work incorporates reviewed changes from [y4my4my4m v4](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG/releases/tag/v10.0.0-dev-fork-y4my4my4m-v4),
commit `7b7220bb` (GPL-3.0), alongside this branch's feature ownership and pipeline refactor.
That source described its native Vulkan split as untested; its performance reports are not an
A/B benchmark or a gameplay validation of this combined branch.

## Native path

- `IFeature_Vk` owns its NR shader/model, like RCAS and output scaling. Pre/post placement uses the
  same reverse setup and forward dispatch contract. Native DLSS SR and RR both follow this path.
- `RunBeforeSR` selects owned colour scratch before SR or RR+SR; parameter pointers and layouts
  restore on failure. Active origin-zero colour rectangles are respected. Invalid pre-colour
  shapes select the ordinary post-upscale path where supported.
- Depth and motion have independent valid rectangles, origins and storage-capability metadata.
  See [NR-MOTION-METADATA.md](NR-MOTION-METADATA.md).
- Model passes have independent features/history, ping-pong answers and compose once. Profiles,
  working resolution, supersampling filters, skin controls and reversible composition are shared
  with D3D12. A GPU event keeps model creation separate from evaluation.
- Vulkan-to-D3D12 bridges inherit D3D12 NR and do not dispatch native NR again.
- Exposure readback and finished-picture presentation are available in native Vulkan. Exposure
  scanning and deferred private-upscaler composition use D3D12, including its bridges.
  See [finished-picture routes](NR-FINISHED-BRIDGES.md).

Enable NR before creating the Vulkan device and swapchain so optional extensions and transfer
usage can be prepared. Enabling it later may require restarting the game. Resources remain lazy
once that preparation is available.

For a native Vulkan starting point:

```ini
[Upscalers]
VulkanUpscaler=dlss
[DlssNr]
Enabled=true
RunBeforeSR=true
DeferredDLSS=false
Passes=1
WorkingScale=1.0
```

Select the quality mode in the game. Working scale is relative to NR's active input, so pre-SR
and post-SR placement can have different costs at the same percentage.

## Separate compatibility work

The useful Vulkan/Proton device-identification, graphics-state, parameter-restoration and menu
fixes remain documented in [COMPATIBILITY-CHANGES.md](COMPATIBILITY-CHANGES.md), with attribution.
These fixes do not require a bundled NVIDIA runtime.

## Validation limits

Historical testing included Release builds and execution of the production Vulkan composition
shader on RTX 5090 at odd/padded dimensions. Those are shader checks, not a full game/NGX model
test. Native Vulkan pre/post A/B, changes in dynamic resolution, multipass profiles, RR and Proton
still need gameplay validation on the final combined build. No performance gain is inferred from
compilation or from another fork's results.
