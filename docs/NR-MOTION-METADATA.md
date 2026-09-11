# NR motion-vector metadata integration

Adapted from [cmh1448's commit 6446cc8](https://github.com/cmh1448/OptiScaler_DLSSNR/commit/6446cc8dcc76e2c2b786384300e217f352b2c18b),
published in [v0.2.0-motion-vector-fixed](https://github.com/cmh1448/OptiScaler_DLSSNR/releases/tag/v0.2.0-motion-vector-fixed).
That release reports a Stellar Blade test; it does not validate this fork's combined build.

The integration forwards independent depth and motion-vector origins and valid sizes to NR.
`MVLowRes` chooses render-resolution vectors; otherwise the DLSS output extent is used,
including when this fork runs NR on the smaller pre-SR colour input. Each region is bounded
against its own allocation. Empty regions skip NR. Missing dimensions fall back to that
resource's available region rather than borrowing depth dimensions for motion vectors.

DX12 applies the working-resolution conversion independently on X and Y; native Vulkan
already did so and retains it. Pre-SR placement, per-pass histories,
colour composition, RR placement and frame-generation ownership are preserved. Each NR pass
receives the same guide metadata. The DX11/Vulkan-to-DX12 bridges retain their parameter blocks
while substituting shared resources, so they reach the same DX12 handling.

The driver-dispatched model context carries the additional guide metadata through its NGX parameter
map. The former helper/export ABI is removed. Install the complete release for OptiScaler's ordinary
backend dependencies; no separate NR helper is required.

Earlier validation, before mandatory driver dispatch: Release x64 build; headless `nr_guides_smoke.cpp` covering
render/output-resolution vectors, padded allocations, independent origins, clipping and invalid
regions; existing padded-colour GPU copy tests on WARP. No new model weights or shaders are
introduced by this integration. Visual shimmering improvement and native Vulkan gameplay
remain unverified in the combined build.
